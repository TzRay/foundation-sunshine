#pragma once
#include "encoded_audio_source.h"
#include "mat_capture_protocol.h"
namespace mat_capture {
  // REST is accepted only on an explicitly negotiated pipe. Consume complete
  // old records until the descriptor barrier; no old bytes reach the new epoch.
  template <class Io>
  void restart_barrier(Io io, uint64_t &next, uint64_t &last_qpc, const Descriptor &expected) {
    using spatial_audio::require;
    for (;;) {
      uint8_t prefix[8];
      io(prefix, sizeof(prefix), false);
      if (!memcmp(prefix, "MCAPv002", sizeof(prefix))) {
        uint8_t header[descriptor_bytes];
        memcpy(header, prefix, sizeof(prefix));
        io(header + sizeof(prefix), sizeof(header) - sizeof(prefix), false);
        Descriptor received;
        require(decode_descriptor(header, received) && received.kind == expected.kind &&
          received.publisher_pid == expected.publisher_pid && received.publisher_created == expected.publisher_created &&
          received.engine_pid == expected.engine_pid && received.engine_created == expected.engine_created &&
          received.encoder == expected.encoder && received.epoch == expected.epoch && received.qpc_hz == expected.qpc_hz,
          "Native MAT restart source identity changed");
        next = last_qpc = 0;
        return;
      }
      uint8_t header[record_bytes];
      memcpy(header, prefix, sizeof(prefix));
      io(header + sizeof(prefix), sizeof(header) - sizeof(prefix), false);
      require(valid_record(header, next, last_qpc) && mat_get32(header + 4) == cycle_kind,
        "Native MAT restart cannot recover a damaged/ended source");
      std::vector<uint8_t> bytes(MAT_WIRE_CYCLE);
      io(bytes.data(), bytes.size(), false);
      require(mat_hash(bytes.data(), bytes.size()) == mat_get32(header + 28), "Native MAT restart old record changed");
      spatial_audio::validate_carrier(bytes);
      last_qpc = mat_get64(header + 16);
      ++next;
    }
  }
  // Wire parsing only. NativeMatPipeSource separately authenticates the publisher,
  // audiodg identity and non-test descriptor before allowing this reader to run.
  template <class Io>
  spatial_audio::Read
  read_record(Io io, uint64_t &next, uint64_t &last_qpc, uint64_t epoch, uint64_t domain, spatial_audio::Frame &out) {
    using spatial_audio::require;
    uint8_t h[record_bytes];
    io(h, sizeof(h), false);
    require(valid_record(h, next, last_qpc), "MAT capture record sequence/time/bounds");
    uint32_t kind = mat_get32(h + 4);
    last_qpc = mat_get64(h + 16);
    require(kind != error_kind, "Native MAT capture failed; never synthesize EOS");
    if (kind == end_kind) {
      require(next > 0 && mat_get32(h + 28) == mat_hash(nullptr, 0), "Empty/invalid live MAT end");
      uint8_t ack[end_ack_bytes];
      encode_end_ack(ack, next);
      io(ack, sizeof(ack), true);
      return spatial_audio::Read::ended;
    }
    spatial_audio::Frame f;
    f.bytes.resize(spatial_audio::carrier_bytes);
    io(f.bytes.data(), f.bytes.size(), false);
    require(mat_hash(f.bytes.data(), f.bytes.size()) == mat_get32(h + 28), "MAT capture bytes changed");
    spatial_audio::validate_carrier(f.bytes);
    require(next < uint64_t(INT64_MAX) / 960, "Live MAT source counter overflow");
    f.epoch = epoch;
    f.domain = domain;
    f.sequence = next;
    f.sample_start = next * 960;
    ++next;
    out = std::move(f);
    return spatial_audio::Read::frame;
  }
}  // namespace mat_capture
