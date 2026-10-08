"""Relocated CI packages must retain the original GNU runtime identity."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('glibc_profiles', ROOT / 'tests/userland/glibc/profiles.py')
profiles = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profiles)


class RelocatedProfile(unittest.TestCase):
    def test_relocation_keeps_version_and_rejects_changed_input(self):
        for arch, prefix, library in (
            ('riscv', '/usr', 'riscv64-linux-gnu/lib'),
            ('loongarch', '/opt/loongarch64-tools', 'target/usr/lib64'),
        ):
            with self.subTest(arch=arch), tempfile.TemporaryDirectory() as directory:
                base = Path(directory)
                tools = base / 'tools'
                cc = tools / 'bin/model-gcc'
                libc = tools / library / 'libc.so.6'
                cc.parent.mkdir(parents=True)
                libc.parent.mkdir(parents=True)
                cc.write_bytes(b'compiler identity')
                cc.chmod(0o755)
                version = '2.44' if arch == 'riscv' else '2.42'
                libc.write_bytes(f'stable release version {version}'.encode())
                manifest = {
                    'glibc_version': version,
                    'tools': {prefix + '/bin/model-gcc': profiles.digest(cc)},
                    'runtime': {prefix + '/' + library + '/libc.so.6': profiles.digest(libc)},
                }
                file = base / ('inputs.json' if arch == 'riscv' else 'inputs-loongarch.json')
                file.write_text(json.dumps(manifest))
                # RV packages retain their /usr member prefix when extracted.
                package = base if arch == 'loongarch' else base / 'package'
                if arch == 'riscv':
                    package.mkdir()
                    tools.rename(package / 'usr')
                    cc = package / 'usr/bin/model-gcc'
                    libc = package / 'usr' / library / 'libc.so.6'
                else:
                    package = tools
                with patch.object(profiles, 'HERE', base):
                    result = profiles.checked_inputs(arch, package)
                    self.assertEqual(result['glibc_version'], version)
                    self.assertIn(str(cc), result['tools'])
                    self.assertIn(str(libc), result['runtime'])
                    libc.write_bytes(b'changed runtime')
                    with self.assertRaisesRegex(RuntimeError, 'identity mismatch'):
                        profiles.checked_inputs(arch, package)


if __name__ == '__main__':
    unittest.main()
