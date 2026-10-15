"""The native workflow must select RV64 from a mixed OpenSBI package."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]


class FirmwareSelection(unittest.TestCase):
    def select(self, work, relatives):
        firmware = []
        for relative in relatives:
            path = work / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(relative.encode())
            firmware.append(path)
        binary = work / 'bin'
        binary.mkdir()
        (binary / 'python3').symlink_to(sys.executable)
        dpkg = binary / 'dpkg-query'
        dpkg.write_text('#!/bin/sh\nif [ "$1" = -L ]; then\n'
                        + 'printf "%s\\n" ' + ' '.join(shlex.quote(str(p)) for p in firmware)
                        + '\nelse printf "opensbi\\t1.3-1\\n"; fi\n')
        dpkg.chmod(0o755)
        qemu = binary / 'qemu-system-riscv64'
        qemu.write_text('#!/bin/sh\nprintf "QEMU emulator version 8.2.2\\n"\n')
        qemu.chmod(0o755)
        (work / 'tests').symlink_to(ROOT / 'tests', target_is_directory=True)
        workflow = yaml.safe_load((ROOT / '.github/workflows/native-contracts.yml').read_text())
        step = next(step for step in workflow['jobs']['contracts']['steps']
                    if step.get('name') == 'Record and select the actual SMP firmware')
        environment = work / 'github-env'
        result = subprocess.run(['sh', '-e', '-c', step['run']], cwd=work,
                                env=dict(os.environ, PATH=str(binary) + ':' + os.environ['PATH'],
                                         GITHUB_ENV=str(environment)),
                                capture_output=True, text=True)
        return result, environment

    def test_mixed_package_selects_rv64_and_records_actual_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            rv64 = 'usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin'
            result, environment = self.select(work, [
                'usr/lib/riscv32-linux-gnu/opensbi/generic/fw_dynamic.bin', rv64])
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            report = json.loads((work / 'build/riscv-smp/ci-executor.json').read_text())
            self.assertEqual(report['firmware']['path'], str(work / rv64))
            self.assertEqual(report['firmware']['sha256'], hashlib.sha256(rv64.encode()).hexdigest())
            self.assertEqual(environment.read_text(), 'QEMU_RISCV_FIRMWARE=' + str(work / rv64) + '\n')

    def test_missing_or_ambiguous_rv64_does_not_publish_environment(self):
        cases = [
            ['usr/lib/riscv32-linux-gnu/opensbi/generic/fw_dynamic.bin'],
            ['usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin',
             'extra/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin'],
        ]
        for paths in cases:
            with self.subTest(paths=paths), tempfile.TemporaryDirectory() as directory:
                result, environment = self.select(Path(directory), paths)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(environment.exists())


if __name__ == '__main__':
    unittest.main()
