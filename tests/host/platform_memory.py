"""Requested RAM must reach guest commands and every platform case."""
import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
import network_runner
import rng_runner


class MemoryProfiles(unittest.TestCase):
    def test_network_commands_use_both_requested_memories(self):
        with tempfile.TemporaryDirectory() as area:
            root = Path(area)
            (root / 'build/network').mkdir(parents=True)
            for name in ('linux', 'boaros'):
                (root / name).write_bytes(name.encode())
            commands = []

            def compile_program(argv, **unused):
                Path(argv[argv.index('-o') + 1]).write_bytes(b'program')

            def fixture(work, program):
                image = work / 'base.img'
                image.write_bytes(program.read_bytes())
                return image

            def launch(argv, log, budget):
                commands.append(argv)
                log.write_text('NETWORK PASS contract\nexited status=0x0 heap-live=0x0; shutting down\n')

            with patch.object(network_runner, 'ROOT', root), \
                 patch.object(network_runner, 'PROFILES', {'riscv': SimpleNamespace(musl_flags=lambda cc: [])}), \
                 patch.object(network_runner.subprocess, 'run', side_effect=compile_program), \
                 patch.object(network_runner.subprocess, 'check_output', return_value='QEMU fixture'), \
                 patch.object(network_runner.harness, 'fixture', side_effect=fixture), \
                 patch.object(network_runner.harness, 'fixed_linux_image', return_value=(root / 'linux', {})), \
                 patch.object(network_runner.harness, 'run_logged', side_effect=launch), \
                 patch.object(sys, 'argv', ['network', '--kernel', str(root / 'boaros'), '--memory', '512M', '--memory', '1G']), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(network_runner.main('riscv'), 0)
            self.assertEqual(len(commands), 4)
            self.assertEqual({argv[argv.index('-m') + 1] for argv in commands}, {'512M', '1G'})
            self.assertEqual(len({argv[argv.index('-drive') + 1] for argv in commands}), 4)

    def test_rng_runs_every_mode_and_transport_for_both_memories(self):
        with tempfile.TemporaryDirectory() as area:
            rows = []
            with patch.object(rng_runner, 'run', side_effect=lambda args, profile, platform, memory, transport, mode: rows.append((memory, transport, mode))), \
                 patch.object(sys, 'argv', ['rng', '--memory', '512M', '--memory', '1G', '--output', str(Path(area) / 'out')]), \
                 contextlib.redirect_stdout(io.StringIO()):
                rng_runner.main('riscv')
            self.assertEqual(set(rows), {(ram, transport, mode) for ram in ('512M', '1G')
                                        for transport in ('legacy', 'modern') for mode in ('n', 'a', 'd', 's')})
            self.assertEqual(len(rows), 16)

    def test_environment_rejects_invalid_memory_before_preparation(self):
        result = subprocess.run(['sh', str(ROOT / 'tests/environment-riscv.sh')],
                                env={**os.environ, 'QEMU_MEMORY': 'invalid'}, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('QEMU_MEMORY must be', result.stderr)


if __name__ == '__main__':
    unittest.main()
