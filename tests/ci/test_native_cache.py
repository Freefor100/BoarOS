"""A corrupt cached reference must fail before make can bless it again."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('la_prepare', ROOT / 'tests/loongarch/prepare.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class LinuxCache(unittest.TestCase):
    def test_corrupt_or_missing_image_is_rejected_before_build(self):
        for missing in (False, True):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                build = root / 'build/linux-la'
                build.mkdir(parents=True)
                config = build / '.config'
                config.write_text('CONFIG_16KB_3LEVEL=y\n')
                image = build / 'vmlinux'
                image.write_bytes(b'correct image')
                compiler = {'path': '/model-gcc', 'target': 'loongarch64-model', 'version': 'model'}
                identity = {'revision': 'pin', 'compiler': compiler,
                    'profile': 'la64-16kb-3level-initramfs-v1',
                    'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
                    'configuration_sha256': hashlib.sha256(config.read_bytes()).hexdigest()}
                (build / 'boaros-identity.json').write_text(json.dumps(identity))
                if missing:
                    image.unlink()
                else:
                    image.write_bytes(b'corrupt image')
                with patch.object(prepare, 'ROOT', root), \
                     patch.object(prepare, 'fixed_repository', return_value=(root, 'pin')), \
                     patch.object(prepare, 'compiler_identity', return_value=compiler), \
                     patch.object(prepare.subprocess, 'run') as build_command:
                    with self.assertRaisesRegex(SystemExit, 'image identity'):
                        prepare.prepare(argparse.Namespace(component='linux', profile='core', cross='model-', jobs=1))
                    build_command.assert_not_called()


class QemuNetwork(unittest.TestCase):
    def test_real_backend_probe_accepts_user_and_requires_the_target_machine(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'qemu'
            image.write_text('#!/bin/sh\ntest "$1 $2 $3 $4" = "-machine virt -netdev help" || exit 2\nprintf "Available netdev backend types:\\nuser\\ntap\\n"\n')
            image.chmod(0o755)
            prepare.require_user_network(image)

    def test_qemu_without_user_backend_is_not_published_or_reused(self):
        for cached in (False, True):
            with self.subTest(cached=cached), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                build = root / 'build/qemu-la'
                build.mkdir(parents=True)
                image = build / 'qemu-system-loongarch64'
                image.write_text('#!/bin/sh\nif [ "$3" = -netdev ]; then printf "Available netdev backend types:\\nsocket\\ntap\\n"; else echo "QEMU model"; fi\n')
                image.chmod(0o755)
                compiler = {'path': '/model-cc', 'target': 'host', 'version': 'model'}
                stamp = build / 'boaros-identity.json'
                identity = {'revision': 'pin', 'compiler': compiler, 'profile': prepare.QEMU_OPTIONS,
                            'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest()}
                if cached:
                    stamp.write_text(json.dumps(identity))
                original = stamp.read_bytes() if cached else None
                real_run = subprocess.run
                def command(argv, **kwargs):
                    if str(argv[0]) == str(root / 'configure') or argv[0] == 'ninja':
                        return subprocess.CompletedProcess(argv, 0)
                    return real_run(argv, **kwargs)
                with patch.object(prepare, 'ROOT', root), \
                     patch.object(prepare, 'fixed_repository', return_value=(root, 'pin')), \
                     patch.object(prepare, 'compiler_identity', return_value=compiler), \
                     patch.object(prepare.subprocess, 'run', side_effect=command):
                    with self.assertRaisesRegex(SystemExit, 'user network backend'):
                        prepare.prepare(argparse.Namespace(component='tools', jobs=1))
                self.assertEqual(stamp.read_bytes() if stamp.exists() else None, original)


if __name__ == '__main__':
    unittest.main()
