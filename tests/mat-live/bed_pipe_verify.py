"""Opt-in native bed publisher smoke: private pipe, real source, no network/HAL claim."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pipe", required=True)
    parser.add_argument("--capture-log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tone-probe", type=Path)
    parser.add_argument("--endpoint", default="default")
    parser.add_argument("--source-kind", choices=("loopback-bed", "system-native-output"), default="loopback-bed")
    parser.add_argument("--max-cycles", type=int, default=0,
                        help="Close only this lease after N cycles; 0 waits for native END")
    parser.add_argument("--carrier-output", type=Path,
                        help="Save the unmodified IEC carrier to a new local file")
    args = parser.parse_args()
    if not 0 <= args.max_cycles <= 1500:
        parser.error("--max-cycles must be 0..1500")
    deadline = time.monotonic() + 30
    while True:
        log = args.capture_log.read_text(encoding="utf8", errors="replace") if args.capture_log.exists() else ""
        if "BED_FAILED" in log or "\nFAILED " in log:
            raise RuntimeError(log)
        if f"NATIVE_MAT_READY source={args.source_kind}" in log:
            break
        if time.monotonic() > deadline:
            raise TimeoutError("Native publisher did not become ready")
        time.sleep(.02)

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                  wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    kernel.WriteFile.argtypes = kernel.ReadFile.argtypes
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    pipe = kernel.CreateFileW(args.pipe, 0xC0000000, 0, None, 3, 0, None)
    if pipe == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())

    def write(data):
        value = ctypes.create_string_buffer(data)
        count = wintypes.DWORD()
        if not kernel.WriteFile(pipe, value, len(data), ctypes.byref(count), None) or count.value != len(data):
            raise ctypes.WinError(ctypes.get_last_error())

    def read(size):
        result = bytearray()
        while len(result) < size:
            value = ctypes.create_string_buffer(size - len(result))
            count = wintypes.DWORD()
            if not kernel.ReadFile(pipe, value, len(value), ctypes.byref(count), None) or not count.value:
                raise ctypes.WinError(ctypes.get_last_error())
            result.extend(value.raw[:count.value])
        return bytes(result)

    def fnv(data):
        value = 2166136261
        for byte in data:
            value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
        return value

    tone = None
    tone_log = None
    carrier = None
    ended = False
    try:
        if args.carrier_output:
            carrier = args.carrier_output.open("xb")
        write(b"MCAPOPEN")
        descriptor = read(80)
        assert descriptor[:8] == b"MCAPv002"
        assert struct.unpack_from("<III", descriptor, 8) == (80, 2, 2)
        assert struct.unpack_from("<II", descriptor, 64) == (48000, 61440)
        write(b"MCAPPLAY")
        if args.tone_probe:
            tone_log = args.output.with_suffix(".tone.log").open("wb")
            tone = subprocess.Popen([str(args.tone_probe), "--endpoint", args.endpoint],
                                    stdout=tone_log, stderr=subprocess.STDOUT,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
        digest = hashlib.sha256()
        cycles, last_qpc = 0, 0
        while True:
            header = read(40)
            magic, kind, index, qpc, size, checksum, reserved = struct.unpack("<IIQQIIQ", header)
            assert magic == 0x4359434D and index == cycles and qpc >= last_qpc and qpc > 0 and reserved == 0, {
                "magic": hex(magic), "kind": kind, "index": index, "expectedIndex": cycles,
                "qpc": qpc, "lastQpc": last_qpc, "qpcDelta": qpc - last_qpc,
                "size": size, "reserved": reserved}
            last_qpc = qpc
            if kind == 2:
                assert cycles and size == 0 and checksum == fnv(b"")
                write(b"MCAPDONE" + struct.pack("<Q", cycles))
                ended = True
                break
            assert kind == 1 and size == 61440
            payload = read(size)
            assert checksum == fnv(payload)
            assert payload[:4] == b"\x72\xf8\x1f\x4e" and payload[4] & 31 == 0x16
            digest.update(payload)
            if carrier:
                carrier.write(payload)
            cycles += 1
            if args.max_cycles and cycles >= args.max_cycles:
                break
        if tone:
            assert tone.wait(timeout=5) == 0, "Real PCM tone/idle source check failed"
        result = {"result": "PASS" if ended else "SAMPLED", "endAcknowledged": ended,
                  "cycles": cycles, "bytes": cycles * 61440,
                  "sha256": digest.hexdigest(), "nativeBedPipe": args.source_kind == "loopback-bed",
                  "nativeSystemOutputPipe": args.source_kind == "system-native-output",
                  "authenticatedNetworkTested": False, "hardwarePlaybackVerified": False}
        args.output.write_text(json.dumps(result, indent=2), encoding="utf8")
        print(json.dumps(result))
    finally:
        kernel.CloseHandle(pipe)
        if carrier:
            carrier.close()
        if tone and tone.poll() is None:
            tone.terminate()
            tone.wait(timeout=5)
        if tone_log:
            tone_log.close()


if __name__ == "__main__":
    main()
