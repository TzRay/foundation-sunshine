"""Inspect saved native MAT PCM; never modify/repack a carrier or certify Atmos.

The observed 31 full-band + LFE slot layout is corroborated by the reference at
https://github.com/barralutz/spatial-audio-lab/blob/5c689815680f3ea5b09fb35ea4dbe63a3dc5bfb7/docs/TECHNICAL_NOTES.md
This is a diagnostic for that layout, not a general MAT decoder. Metadata and
its integrity fields are intentionally left uninterpreted.
"""
import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


CYCLE_BYTES = 61440
PCM_PREFIX = bytes.fromhex("079e00098401")
FULL = bytes.fromhex("834143c2082f")
LFE = bytes.fromhex("834140f20b2f")
CHANNELS = ["FL", "FR", "FC", "SL", "SR", "BL", "BR",
            "TFL", "TFR", "TBL", "TBR"] + [f"object-slot-{i}" for i in range(20)] + ["LFE"]
TONES = {"FL": 440, "FR": 550, "FC": 660, "LFE": 80,
         "BL": 880, "BR": 990, "SL": 1100, "SR": 1210}


def unpack_observed_pcm(payload):
    """Walk records by length, so marker-like PCM bytes cannot become records."""
    if not payload.startswith(PCM_PREFIX):
        return None
    planes = [array("h") for _ in CHANNELS]
    cursor = len(PCM_PREFIX)
    metadata_lengths = []
    for half in range(2):
        start = payload.find(b"\x85" + FULL, cursor)
        if start < 0:
            return None
        metadata_lengths.append(start - cursor)
        cursor = start + 1
        # LFE follows FC; the other eleven bed slots precede twenty object slots.
        for slot in [0, 1, 2, 31, *range(3, 31)]:
            marker, frames = (LFE, 120) if slot == 31 else (FULL, 480)
            if payload[cursor:cursor + len(marker)] != marker:
                return None
            cursor += len(marker)
            end = cursor + frames * 2
            if end > len(payload):
                return None
            pcm = array("h", payload[cursor:end])
            if sys.byteorder == "little":
                pcm.byteswap()
            planes[slot].extend(pcm)
            cursor = end
            if payload[cursor:cursor + 1] != b"\xc3":
                return None
            cursor += 1
        if payload[cursor:cursor + 1] != b"\xc5":
            return None
        cursor += 1
    return planes, metadata_lengths


def levels(pcm, rate):
    peak = max(map(abs, pcm), default=0)
    result = {"samples": len(pcm), "rate": rate, "peak": peak / 32768,
              "rms": math.sqrt(sum(x * x for x in pcm) / len(pcm)) / 32768 if pcm else 0}
    # Compare tones in the loudest 200 ms window; silence around the one-second
    # probe must not reduce the measured amplitude or hide a swapped channel.
    width = rate // 5
    if peak and len(pcm) >= width:
        start = max(range(0, len(pcm) - width + 1, width),
                    key=lambda i: sum(x * x for x in pcm[i:i + width]))
        window = pcm[start:start + width]
        amplitudes = {}
        for hz in TONES.values():
            step = 2 * math.pi * hz / rate
            re = sum(x * math.cos(step * t) for t, x in enumerate(window))
            im = sum(x * math.sin(step * t) for t, x in enumerate(window))
            amplitudes[hz] = 2 * math.hypot(re, im) / width / 32768
        dominant = max(amplitudes, key=amplitudes.get)
        result.update(dominantProbeHz=dominant, probeAmplitude=amplitudes[dominant])
    return result


def inspect(data, expect_tones=False):
    if not data or len(data) % CYCLE_BYTES:
        raise ValueError("Input must contain complete 61440-byte cycles")
    planes = [array("h") for _ in CHANNELS]
    lengths, signatures, metadata_lengths = set(), set(), set()
    observed = 0
    nonzero_padding = 0
    for index, offset in enumerate(range(0, len(data), CYCLE_BYTES)):
        raw = data[offset:offset + CYCLE_BYTES]
        pa, pb, pc, pd = struct.unpack_from("<4H", raw)
        if (pa, pb) != (0xF872, 0x4E1F) or pc & 31 != 0x16 or pc & 0x80:
            raise ValueError(f"Invalid IEC MAT preamble at cycle {index}")
        if pd < 2 or pd > CYCLE_BYTES - 8 or pd % 2:
            raise ValueError(f"Invalid IEC MAT payload length at cycle {index}: {pd}")
        lengths.add(pd)
        nonzero_padding += bool(any(raw[8 + pd:]))
        wire = raw[8:8 + pd]
        logical = bytearray(pd)
        logical[0::2], logical[1::2] = wire[1::2], wire[0::2]
        signatures.add(logical[:12].hex())
        decoded = unpack_observed_pcm(logical)
        if decoded is None:
            continue
        observed += 1
        slots, metadata = decoded
        metadata_lengths.add(tuple(metadata))
        for out, values in zip(planes, slots):
            out.extend(values)
    cycles = len(data) // CYCLE_BYTES
    report = {"result": "INSPECTED", "cycles": cycles, "seconds": cycles / 50,
              "sha256": hashlib.sha256(data).hexdigest(), "iecMatHeadersValid": True,
              "payloadBytes": sorted(lengths), "payloadPrefixes": sorted(signatures),
              "nonzeroPaddingCycles": nonzero_padding, "observedPcmLayoutCycles": observed,
              "metadataRegionLengths": sorted(metadata_lengths),
              "matVersionVerified": False, "atmosMetadataVerified": False,
              "hardwarePlaybackVerified": False}
    if observed == cycles:
        report["channels"] = {name: levels(values, 12000 if name == "LFE" else 48000)
                              for name, values in zip(CHANNELS, planes)}
    if expect_tones:
        passed = observed == cycles
        for name, hz in TONES.items():
            info = report.get("channels", {}).get(name, {})
            passed &= info.get("dominantProbeHz") == hz and info.get("probeAmplitude", 0) > .002
        # A bed probe contains neither height audio nor dynamic-object audio.
        passed &= all(report.get("channels", {}).get(name, {}).get("peak", 1) < .0001
                      for name in CHANNELS if name not in TONES)
        report["bedProbeTonesPassed"] = bool(passed)
        report["result"] = "PCM_TONES_PASS" if passed else "PCM_TONES_FAIL"
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expect-bed-probe-tones", action="store_true",
                        help="Verify all eight tones from bed-source-probe (LFE=80 Hz)")
    args = parser.parse_args()
    report = inspect(args.input.read_bytes(), args.expect_bed_probe_tones)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps({k: v for k, v in report.items() if k != "channels"}))
    return 1 if report.get("bedProbeTonesPassed") is False else 0


if __name__ == "__main__":
    sys.exit(main())
