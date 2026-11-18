"""Real Web UI routes on the caller's isolated host. No native capture source."""
import ctypes
from ctypes import wintypes
import json
import os
import socket
import urllib.request
from urllib.parse import urlsplit


def verify_ui(opener, port, config, config_path, launch_request=None, app_id=None):
    def request(route, body=None):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(f"https://127.0.0.1:{port+1}/api/{route}", data=data,
                                     headers={"Content-Type": "application/json"})
        with opener.open(req, timeout=5) as response:
            if route == "mat/status":
                assert response.headers.get("Cache-Control") == "no-store"
            return json.load(response)

    # Pending connection detects even a short-lived accidental status probe.
    class Overlapped(ctypes.Structure):
        _fields_ = [("internal", ctypes.c_size_t), ("internal_high", ctypes.c_size_t),
                    ("offset", wintypes.DWORD), ("offset_high", wintypes.DWORD), ("event", wintypes.HANDLE)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                        wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    kernel.CreateNamedPipeW.restype = wintypes.HANDLE
    kernel.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.CreateEventW.restype = wintypes.HANDLE
    kernel.ConnectNamedPipe.argtypes = [wintypes.HANDLE, ctypes.POINTER(Overlapped)]
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(Overlapped)]
    kernel.GetOverlappedResult.argtypes = [wintypes.HANDLE, ctypes.POINTER(Overlapped), ctypes.POINTER(wintypes.DWORD), wintypes.BOOL]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    pipe = kernel.CreateNamedPipeW(r"\\.\pipe\SunshineMatLive-active", 3 | 0x40000000 | 0x80000, 0, 1, 64, 64, 0, None)
    if pipe == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    event = kernel.CreateEventW(None, True, False, None)
    overlapped = Overlapped(event=event)
    pending = False
    try:
        if not event:
            raise ctypes.WinError(ctypes.get_last_error())
        connected = kernel.ConnectNamedPipe(pipe, ctypes.byref(overlapped))
        pending = not connected and ctypes.get_last_error() == 997  # ERROR_IO_PENDING
        assert pending
        for _ in range(10):
            assert request("mat/status") == {"state": "unobserved", "last_error": "", "source": "objects", "negotiated_codec": ""}
        original = config_path.read_bytes()
        for invalid in ("invalid", "3"):
            assert request("config", {**config, "mat_live": invalid})["status"] == "false"
            assert config_path.read_bytes() == original
        for invalid in ("pcm", "invalid"):
            assert request("config", {**config, "mat_source": invalid})["status"] == "false"
            assert config_path.read_bytes() == original
        for enabled in (False, True):
            settings = {**config}
            settings["mat_live"] = "true" if enabled else "false"
            settings["mat_source"] = "bed" if enabled else "objects"
            settings.update(mat_live_pipe="retired", mat_live_producer="retired")
            assert request("config", settings)["status"] == "true"
            saved = request("config")
            assert saved["mat_live"] == settings["mat_live"]
            assert saved["mat_source"] == settings["mat_source"]
            assert "mat_live_pipe" not in saved and "mat_live_producer" not in saved
            text = config_path.read_text(encoding="utf8")
            assert "mat_live_pipe" not in text and "mat_live_producer" not in text
            # Saved changes await restart; runtime observation is independent.
            assert request("mat/status") == {"state": "unobserved", "last_error": "", "source": "objects", "negotiated_codec": ""}
        assert kernel.WaitForSingleObject(event, 0) == 258  # no pipe connection
        negotiation = []
        if launch_request:
            # The pipe belongs to this unelevated fixture, so it cannot pass
            # production publisher authentication. No ANNOUNCE/PLAY is sent.
            _, launch = launch_request("launch", {"appid": app_id, "rikey": os.urandom(16).hex(), "rikeyid": 1,
                "mode": "640x360x30", "sops": 0, "localAudioPlayMode": 0, "surroundAudioInfo": 196610,
                "useVdd": 0, "customScreenMode": 0, "hdrMode": 0, "corever": 0,
                "remoteControllersBitmap": 0, "gcmap": 0, "gcpersist": 0})
            try:
                url = urlsplit(launch.findtext("sessionUrl0"))
                assert url.scheme == "rtsp" and url.hostname == "127.0.0.1"
                for headers, expected in (("", 200), ("X-ML-Mat-Live: 1\r\n", 503), ("X-ML-Mat-Live: 0\r\n", 400),
                                          ("X-ML-Mat-Live: 1\r\nX-ML-Mat-Live: 1\r\n", 400)):
                    with socket.create_connection((url.hostname, url.port), timeout=5) as peer:
                        peer.settimeout(5)
                        peer.sendall((f"DESCRIBE {launch.findtext('sessionUrl0')} RTSP/1.0\r\nCSeq: 1\r\n"+headers+"\r\n").encode())
                        response = b""
                        while b"\r\n\r\n" not in response:
                            part = peer.recv(65536)
                            assert part, "Incomplete DESCRIBE response"
                            response += part
                        assert int(response.split(b" ", 2)[1]) == expected, response[:200]
                        negotiation.append(expected)
            finally:
                launch_request("cancel")
    finally:
        if pending:
            kernel.CancelIoEx(pipe, ctypes.byref(overlapped))
            count = wintypes.DWORD()
            kernel.GetOverlappedResult(pipe, ctypes.byref(overlapped), ctypes.byref(count), True)
        if event:
            kernel.CloseHandle(event)
        kernel.CloseHandle(pipe)
    return {"readOnlyPolls": 10, "invalidSwitchesRejected": 2, "invalidSourcesRejected": 2,
            "switchRoundTrips": 2, "legacyParametersRemoved": True, "describeStatusCodes": negotiation,
            "statusPollOpenedCapturePipe": False, "mediaStarted": False, "savedSettingsRequireRestart": True}
