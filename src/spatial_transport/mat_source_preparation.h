#pragma once
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include "mat_live_session.h"
#include <future>

namespace spatial_transport {
  // All socket and launch-ticket access stays on the RTSP event loop. The
  // worker owns only its preparation function, cancellation flag and lease.
  template<class Socket, class Request>
  class MatSourcePreparation {
    using Clock = std::chrono::steady_clock;
    using Live = std::shared_ptr<LiveSession>;
    using Valid = std::function<bool(const Socket &)>;
    using Reply = std::function<void(Socket &, Request &, Live, const char *)>;
    struct Pending {
      std::shared_ptr<Socket> socket;
      Request request;
      Clock::time_point deadline;
      std::shared_ptr<std::atomic_bool> cancel;
      std::future<Live> work;
      Valid valid;
      Reply reply;
      void respond(Live live, const char *error) {
        reply(*socket, request, std::move(live), error);
        boost::system::error_code ec;
        socket->sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket->sock.close(ec);
        socket.reset();
      }
    };
    std::unique_ptr<Pending> pending_;
    std::weak_ptr<std::atomic_bool> owner_;
    boost::asio::steady_timer timer_;

    void poll() {
      using namespace std::chrono_literals;
      auto &p = *pending_;
      if (p.socket && (Clock::now() >= p.deadline || !p.valid(*p.socket))) {
        p.cancel->store(true);
        p.respond({}, "MAT request expired");
      }
      if (p.work.wait_for(0s) == std::future_status::ready) {
        auto done = std::move(pending_);
        if (!done->socket) return;
        Live live;
        try {
          live = done->work.get();
          if (!live) throw std::runtime_error("MAT source unavailable");
        } catch (const std::exception &e) {
          done->respond({}, e.what());
          return;
        }
        done->respond(std::move(live), nullptr);
        return;
      }
      const auto next = Clock::now() + 20ms;
      timer_.expires_at(p.socket ? (std::min)(p.deadline, next) : next);
      timer_.async_wait([this](const boost::system::error_code &ec) { if (!ec) poll(); });
    }

  public:
    explicit MatSourcePreparation(boost::asio::io_context &io): timer_(io) {}

    template<class Prepare>
    bool begin(const std::shared_ptr<Socket> &socket, Request &&req, Clock::time_point deadline,
               Prepare prepare, Valid valid, Reply reply) {
      if (pending_ || !owner_.expired()) return false;
      auto cancel = std::make_shared<std::atomic_bool>(false);
      auto work = std::async(std::launch::async, [cancel, prepare = std::move(prepare)] {
        auto live = prepare([cancel] { return cancel->load(); });
        if (live) live->reservation = cancel;
        return live;
      });
      pending_ = std::make_unique<Pending>(Pending {socket, std::move(req), deadline, cancel,
        std::move(work), std::move(valid), std::move(reply)});
      owner_ = cancel;
      // Moonlight keeps its one-request TCP connection open while awaiting the reply.
      socket->sock.async_wait(boost::asio::ip::tcp::socket::wait_read,
        [this, peer = std::weak_ptr<Socket>(socket)](const boost::system::error_code &ec) {
          if (ec != boost::asio::error::operation_aborted && pending_ && pending_->socket && pending_->socket == peer.lock()) {
            pending_->cancel->store(true);
            pending_->socket.reset();
          }
        });
      poll();
      return true;
    }

    // Called after joining the RTSP event loop. Destroying the future joins
    // preparation before its socket, request and ticket are discarded.
    void shutdown() {
      if (pending_) pending_->cancel->store(true);
      pending_.reset();
    }
  };
}
