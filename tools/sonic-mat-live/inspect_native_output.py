"""Export diagnostic callback bytes without re-encoding or claiming Atmos validity."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

FILE = struct.Struct("<4I2Q")
RECORD = struct.Struct("<4Q3Ii2I")
PREAMBLE = bytes.fromhex("72f81f4e")
CYCLE = 61440


def valid_header(data, at):
    if at + 8 > len(data) or data[at:at + 4] != PREAMBLE:
        return False
    pc, pd = struct.unpack_from("<HH", data, at + 4)
    return pc & 31 == 0x16 and not pc & 0x80 and 2 <= pd <= CYCLE - 8 and pd % 2 == 0


def aligned_cycles(data):
    # Keep the raw export as the authority. Only trim an explicitly reported
    # partial prefix/suffix; never resync past a malformed complete cycle.
    start = data.find(PREAMBLE)
    if start < 0 or not valid_header(data, start):
        return None
    count = (len(data) - start) // CYCLE
    if not count or any(not valid_header(data, start + i * CYCLE) for i in range(count)):
        return None
    end = start + count * CYCLE
    return start, end, count


def inspect(capture, output, compare=None):
    groups = {}
    with capture.open("rb") as stream:
        header = stream.read(FILE.size)
        if len(header) != FILE.size:
            raise ValueError("Truncated capture header")
        magic, version, source, pid, created, frequency = FILE.unpack(header)
        if magic != 0x3154554F or version not in (1, 2) or source not in (1, 2) or not frequency:
            raise ValueError("Unsupported capture header")
        sequence = 0
        while raw := stream.read(RECORD.size):
            if len(raw) != RECORD.size:
                raise ValueError("Truncated callback record")
            seq, qpc, wrapper, implementation, frames, capacity, produced, result, tid, endpoint_slot = RECORD.unpack(raw)
            if seq != sequence or produced > min(capacity, 65536) or result < 0 or not 0 <= endpoint_slot <= (35 if version == 2 else 0):
                raise ValueError(f"Invalid callback record {sequence}")
            payload = stream.read(produced)
            if len(payload) != produced:
                raise ValueError(f"Truncated callback payload {sequence}")
            key = wrapper, implementation, endpoint_slot
            if key not in groups:
                groups[key] = {"data": bytearray(), "records": 0, "firstQpc": qpc,
                               "lastQpc": qpc, "sizes": Counter(), "frames": Counter()}
            group = groups[key]
            group["data"].extend(payload)
            group["records"] += 1
            group["lastQpc"] = qpc
            group["sizes"][produced] += 1
            group["frames"][frames] += 1
            sequence += 1
    summary_file = capture.with_name("capture-summary.json")
    summary = json.loads(summary_file.read_text(encoding="utf-8-sig")) if summary_file.exists() else None
    if summary:
        with capture.open("rb") as stream:
            capture_hash = hashlib.file_digest(stream, "sha256").hexdigest()
        if (summary.get("records") != sequence or summary.get("bytes") != sum(len(g["data"]) for g in groups.values()) or
                summary.get("captureSha256") != capture_hash):
            raise ValueError("Capture summary does not match this record file")
    reference = compare.read_bytes() if compare else None
    if reference is not None and (not reference or len(reference) % CYCLE or
                                  any(not valid_header(reference, i) for i in range(0, len(reference), CYCLE))):
        raise ValueError("Comparison must contain complete IEC MAT-family cycles")
    output.mkdir(parents=False, exist_ok=False)
    report = {"source": "system-callback" if source == 1 else "own-process-test",
              "pid": pid, "created": created, "records": sequence,
              "captureSummary": summary, "groups": [], "atmosVerified": False,
              "completeCapture": bool(source == 1 and summary and summary.get("complete") and summary.get("restored") and
                                      not any(summary.get(k, 1) for k in ("dropped", "invalid", "failedCalls", "hookError"))),
              "note": "Grouped by observed instance addresses; endpoint, renderer resets and receiver recognition are not inferred."}
    for number, ((wrapper, implementation, endpoint_slot), group) in enumerate(groups.items()):
        data = group["data"]
        name = f"instance-{number:02d}"
        (output / f"{name}.raw").write_bytes(data)
        item = {"name": name, "wrapper": hex(wrapper), "implementation": hex(implementation),
                "endpointSlot": endpoint_slot, "endpoint": summary.get("endpoint") if endpoint_slot and summary else None,
                "records": group["records"], "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "callbackBytes": dict(group["sizes"]), "inputFrames": dict(group["frames"]),
                "observedSpanSeconds": (group["lastQpc"] - group["firstQpc"]) / frequency,
                "iecMatFamilyAligned": False}
        aligned = aligned_cycles(data)
        if aligned:
            first, end, count = aligned
            cycles = data[first:end]
            (output / f"{name}.iec").write_bytes(cycles)
            item.update(iecMatFamilyAligned=True, cycles=count,
                        omittedPrefixBytes=first, omittedSuffixBytes=len(data) - end,
                        pcValues=sorted({struct.unpack_from("<H", cycles, at + 4)[0] for at in range(0, len(cycles), CYCLE)}),
                        pdValues=sorted({struct.unpack_from("<H", cycles, at + 6)[0] for at in range(0, len(cycles), CYCLE)}))
            if reference is not None:
                left, right = cycles[:CYCLE], reference[:CYCLE]
                differences = [i for i, (a, b) in enumerate(zip(left, right)) if a != b]
                item["firstCycleComparison"] = {"reference": str(compare.resolve()),
                    "differingBytes": len(differences), "firstDifferenceOffsets": differences[:32],
                    "interpretation": "Byte differences include PCM, timing and metadata; they do not identify an Atmos defect."}
        report["groups"].append(item)
    (output / "inspection.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path, help="Fresh export directory")
    parser.add_argument("--compare", type=Path, help="Saved IEC carrier; compares first full cycles only")
    args = parser.parse_args()
    print(json.dumps(inspect(args.capture, args.output, args.compare), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
