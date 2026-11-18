import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from inspect_native_output import FILE, RECORD, CYCLE, aligned_cycles, inspect


class CaptureInspection(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.capture = self.root / "native-output.mocap"
        self.cycle = bytes.fromhex("72f81f4e16000200") + bytes(CYCLE - 8)

    def tearDown(self):
        self.temp.cleanup()

    def write(self, entries, source=2, version=1, endpoint_slot=0):
        data = FILE.pack(0x3154554F, version, source, 1, 0, 1000)
        for seq, (instance, payload) in enumerate(entries):
            data += RECORD.pack(seq, seq + 1, instance, instance + 1, 480, 65536, len(payload), 0, 1, endpoint_slot) + payload
        self.capture.write_bytes(data)

    def test_byte_exact_separate_instances_and_test_provenance(self):
        self.write([(10, self.cycle[:30720]), (20, b"other"), (10, self.cycle[30720:])])
        report = inspect(self.capture, self.root / "out")
        self.assertEqual((self.root / "out/instance-00.raw").read_bytes(), self.cycle)
        self.assertEqual((self.root / "out/instance-01.raw").read_bytes(), b"other")
        self.assertEqual(report["source"], "own-process-test")
        self.assertFalse(report["completeCapture"])
        self.assertFalse(report["atmosVerified"])

    def test_partial_edges_and_broken_complete_cycle(self):
        self.assertEqual(aligned_cycles(b"prefix" + self.cycle * 2 + b"tail"), (6, 6 + 2 * CYCLE, 2))
        self.assertIsNone(aligned_cycles(self.cycle + bytes(CYCLE) + self.cycle))

    def test_v2_endpoint_binding_and_legacy_reserved_field(self):
        self.write([(10, self.cycle)], version=2, endpoint_slot=3)
        report = inspect(self.capture, self.root / "out")
        self.assertEqual(report["groups"][0]["endpointSlot"], 3)
        self.write([(10, self.cycle)], version=1, endpoint_slot=3)
        with self.assertRaisesRegex(ValueError, "Invalid callback record"):
            inspect(self.capture, self.root / "bad")

    def test_truncated_payload_rejected(self):
        self.write([(10, self.cycle)])
        self.capture.write_bytes(self.capture.read_bytes()[:-1])
        with self.assertRaisesRegex(ValueError, "Truncated callback payload"):
            inspect(self.capture, self.root / "out")

    def test_summary_bound_to_file_and_never_certifies_atmos(self):
        self.write([(10, self.cycle)], source=1)
        summary = dict(records=1, bytes=CYCLE, captureSha256=hashlib.sha256(self.capture.read_bytes()).hexdigest(),
                       complete=True, restored=True, dropped=0, invalid=0, failedCalls=0, hookError=0)
        path = self.root / "capture-summary.json"
        path.write_text(json.dumps(summary), encoding="utf-8")
        report = inspect(self.capture, self.root / "out")
        self.assertTrue(report["completeCapture"])
        self.assertFalse(report["atmosVerified"])
        summary["captureSha256"] = "wrong"
        path.write_text(json.dumps(summary), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "does not match"):
            inspect(self.capture, self.root / "bad")


if __name__ == "__main__":
    unittest.main()
