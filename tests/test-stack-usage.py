#!/usr/bin/env python3
"""Regression cases for the compiler stack-bound gate."""
import subprocess
import tempfile
import unittest
from pathlib import Path


class StackUsageGate(unittest.TestCase):
    def check_report(self, line):
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "fixture.su").write_text(line)
            return subprocess.run(
                ["python3", str(Path(__file__).with_name("stack-usage.py")),
                 directory, "--stack-bytes", "4096", "--trap-frame-bytes", "288"],
                capture_output=True, text=True,
            )

    def test_static_frame_limit_includes_assembly_and_reserve(self):
        self.assertEqual(self.check_report("f.c:1:1:f\t2768\tstatic\n").returncode, 0)
        result = self.check_report("f.c:1:1:f\t2769\tstatic\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exceeds", result.stderr)

    def test_unbounded_dynamic_frame_is_rejected(self):
        result = self.check_report("f.c:1:1:f\t16\tdynamic\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("uncontrolled", result.stderr)
        self.assertEqual(self.check_report("f.c:1:1:f\t64\tdynamic,bounded\n").returncode, 0)

    def test_missing_records_do_not_claim_success(self):
        self.assertNotEqual(self.check_report("").returncode, 0)
        self.assertNotEqual(self.check_report("broken record\n").returncode, 0)


if __name__ == "__main__":
    unittest.main()
