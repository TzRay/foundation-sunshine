"""Real trusted publisher lease and audio routing, without ANNOUNCE/PLAY or HDMI output."""
import json
import os
import re
import socket
import subprocess
import time
import urllib.request
from urllib.parse import urlsplit


def verify_managed_bed(opener, port, request, app_id, endpoint, probe, output):
    def status():
        with opener.open(f"https://127.0.0.1:{port+1}/api/mat/status", timeout=5) as response:
            assert response.headers.get("Cache-Control") == "no-store"
            return json.load(response)

    def defaults(name):
        result = subprocess.run([str(probe), "--list"], capture_output=True, timeout=10)
        text = result.stdout.decode("utf8", errors="replace")
        (output / f"{name}.log").write_text(text, encoding="utf8")
        assert result.returncode == 0, text[-1000:]
        return {match[0]: tuple(map(int, match[1:])) for match in re.findall(
            r"endpoint=(\S+) state=0x[0-9a-fA-F]+ defaultConsole=(\d) defaultMultimedia=(\d) defaultCommunications=(\d)", text)}

    before = defaults("defaults-before")
    ready = status()
    assert ready["source"] == "bed" and ready["state"] == "ready" and ready["negotiated_codec"] == "", ready
    publisher = ready["publisher_pid"]
    _, launch = request("launch", {"appid": app_id, "rikey": os.urandom(16).hex(), "rikeyid": 1,
        "mode": "640x360x30", "sops": 0, "localAudioPlayMode": 0, "surroundAudioInfo": 196610,
        "useVdd": 0, "customScreenMode": 0, "hdrMode": 0, "corever": 0,
        "remoteControllersBitmap": 0, "gcmap": 0, "gcpersist": 0})
    try:
        url = urlsplit(launch.findtext("sessionUrl0"))
        assert url.scheme == "rtsp" and url.hostname == "127.0.0.1"
        with socket.create_connection((url.hostname, url.port), timeout=5) as peer:
            peer.settimeout(5)
            peer.sendall((f"DESCRIBE {launch.findtext('sessionUrl0')} RTSP/1.0\r\nCSeq: 1\r\nX-ML-Mat-Live: 1\r\n\r\n").encode())
            response = bytearray()
            while b"\r\n\r\n" not in response:
                part = peer.recv(65536)
                assert part, "Incomplete DESCRIBE response"
                response.extend(part)
            header, body = bytes(response).split(b"\r\n\r\n", 1)
            assert header.startswith(b"RTSP/1.0 200 "), header.decode(errors="replace")
            fields = {k.lower(): v.strip() for k, v in (line.split(":", 1) for line in header.decode().split("\r\n")[1:] if ":" in line)}
            if "content-length" in fields:
                size = int(fields["content-length"])
                while len(body) < size:
                    part = peer.recv(size - len(body))
                    assert part, "Incomplete MAT SDP"
                    body += part
            else:
                # Sunshine closes each plaintext RTSP transaction and sends
                # the payload separately from the response headers.
                while True:
                    part = peer.recv(65536)
                    if not part:
                        break
                    body += part
            sdp = body.decode()
            for value in ("a=x-ss-audio.mat-live:1", "a=x-ss-audio.mat-live-carrier:iec61937-192000-8-s16le",
                          "a=x-ss-audio.mat-live-clock:48000", "a=x-ss-audio.mat-live-crypto:aes128-gcm-hkdf-sha256"):
                assert value in sdp, sdp
            (output / "managed-mat.sdp").write_text(sdp, encoding="utf8")
            during = defaults("defaults-during")
            assert during[endpoint][1] == 1, during
            observed = status()
            assert observed["state"] == "ready" and observed["negotiated_codec"] == "", observed
    finally:
        request("cancel")
    deadline = time.monotonic() + 6
    while True:
        after = defaults("defaults-after")
        if after == before:
            break
        assert time.monotonic() < deadline, (before, after)
        time.sleep(.2)
    final = status()
    assert final["state"] == "ready" and final["publisher_pid"] == publisher and not final["last_error"], final
    return {"trustedNativeLease": True, "describeStatus": 200, "matProfile": "mat-live-v1",
            "outputSelectedAndRestored": True, "publisherRetained": True, "apiReadyWithoutStreaming": True,
            "mediaStarted": False, "hardwarePlaybackVerified": False}
