"""A corrupt cached reference must fail before make can bless it again."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
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


if __name__ == '__main__':
    unittest.main()
