import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
import environment


class EnvironmentTests(unittest.TestCase):
    def test_tree_identity_detects_modification_addition_removal_and_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            file = root / 'linux.h'
            file.write_bytes(b'header')
            initial = environment.tree_sha(root)
            file.write_bytes(b'altered')
            self.assertNotEqual(initial, environment.tree_sha(root))
            file.write_bytes(b'header')
            self.assertEqual(initial, environment.tree_sha(root))
            file.rename(root / 'asm.h')
            self.assertNotEqual(initial, environment.tree_sha(root))
            (root / 'linux.h').write_bytes(b'header')
            self.assertNotEqual(initial, environment.tree_sha(root))

    def test_wrong_reference_fails_before_execution(self):
        with patch.object(environment, 'capture', return_value='wrong'), \
             patch.object(environment.subprocess, 'run') as run:
            with self.assertRaisesRegex(RuntimeError, 'expected'):
                environment.pinned('linux', environment.LINUX_REVISION)
            run.assert_not_called()

    def test_feature_comparison_ignores_date_but_detects_disabled_applet(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / 'config'
            config.write_text('# date one\nCONFIG_TC=y\n# CONFIG_PIE is not set\n')
            initial = environment.config_settings(config)
            config.write_text('# date two\nCONFIG_TC=y\n# CONFIG_PIE is not set\n')
            self.assertEqual(initial, environment.config_settings(config))
            config.write_text('# date two\n# CONFIG_TC is not set\n# CONFIG_PIE is not set\n')
            self.assertNotEqual(initial, environment.config_settings(config))

    def test_reference_manifest_is_single_source(self):
        import json
        profile = json.loads(environment.INPUTS.read_text())['busybox_uapi']
        self.assertNotIn('archive_url', profile)
        self.assertNotIn('archive_sha256', profile)
        row = environment.reference(profile['archive_reference'], 'file')
        self.assertEqual(row[2], 'https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.tar.xz')
        self.assertEqual(len(row[4]), 64)
        with tempfile.TemporaryDirectory() as temporary:
            manifest = Path(temporary) / 'sources.tsv'
            manifest.write_text('\t'.join(row) + '\n' + '\t'.join(row) + '\n')
            with patch.object(environment, 'MANIFEST', manifest):
                with self.assertRaisesRegex(RuntimeError, 'duplicate'):
                    environment.reference(row[1], 'file')
            manifest.write_text('\t'.join(row) + '\n')
            with patch.object(environment, 'MANIFEST', manifest):
                with self.assertRaisesRegex(RuntimeError, 'wrong-kind'):
                    environment.reference(row[1], 'snapshot')

    def test_build_environment_prevents_host_header_and_compiler_injection(self):
        with patch.dict(environment.os.environ, {'CPATH': '/host/include',
                         'REALGCC': '/wrong/gcc', 'MAKEFLAGS': 'CC=wrong'}):
            result = environment.build_environment()
        self.assertNotIn('CPATH', result)
        self.assertNotIn('MAKEFLAGS', result)
        self.assertEqual(result['REALGCC'], 'riscv64-linux-gnu-gcc')
        self.assertEqual(result['KCONFIG_NOTIMESTAMP'], '1')
        self.assertEqual(result['LC_ALL'], 'C')

    def test_failed_build_propagates_no_success_stub(self):
        import subprocess
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(subprocess.CalledProcessError):
                environment.run(['false'], Path(temporary) / 'build.log')


if __name__ == '__main__':
    unittest.main()
