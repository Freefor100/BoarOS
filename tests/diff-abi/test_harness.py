"""Protocol checks must fail closed, including infrastructure failures."""
import tempfile
import json
from types import SimpleNamespace
from unittest.mock import patch
import harness
import unittest
from pathlib import Path
from harness import normalize, compare, run_logged

GOOD = 'ABI BEGIN 1\nABI result 0 0 3 1 0 616263\nABI END 1\n'

class ProtocolTests(unittest.TestCase):
    def test_only_boot_noise_and_crlf_are_ignored(self):
        self.assertEqual(normalize('boot\r\n' + GOOD.replace('\n', '\r\n'), ['result']), normalize(GOOD, ['result']))

    def test_missing_duplicate_and_malformed_records_fail(self):
        for text in ('', GOOD.replace('ABI result 0 0 3 1 0 616263\n', ''),
                     GOOD.replace('ABI END 1', 'ABI result 0 0 3 1 0 616263\nABI END 1'),
                     GOOD.replace('ABI END 1', ''), GOOD.replace('616263', 'zz'), GOOD.replace('0 0 3 1', '0 14 3 1')):
            with self.subTest(text=text), self.assertRaises(ValueError):
                normalize(text, ['result'])

    def test_semantic_mismatches_fail(self):
        for changed in ('-14 14 3 1 0 616263', '0 0 4 1 0 616263',
                        '0 0 3 2 0 616263', '0 0 3 1 7 616263', '0 0 3 1 0 616264'):
            with self.subTest(changed=changed):
                self.assertFalse(compare(normalize(GOOD, ['result']),
                    normalize(GOOD.replace('0 0 3 1 0 616263', changed), ['result']))[0])

    def test_inputs_are_snapshotted_before_linux_build(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            kernel, program = base / 'kernel', base / 'program'
            kernel.write_bytes(b'original-kernel')
            program.write_bytes(b'original-program')
            def failing_build():
                self.assertEqual((base / 'build/run/boaros-kernel').read_bytes(), b'original-kernel')
                self.assertEqual((base / 'build/run/cases-rv').read_bytes(), b'original-program')
                kernel.write_bytes(b'new-kernel')
                program.write_bytes(b'new-program')
                raise RuntimeError('build failed')
            with patch.object(harness, 'BUILD', base / 'build'), patch.object(harness, 'linux_build', failing_build):
                with self.assertRaisesRegex(RuntimeError, 'build failed'):
                    harness.run(SimpleNamespace(kernel=kernel, program=program, timeout=1))
            metadata = json.loads((base / 'build/run/metadata.json').read_text())
            self.assertEqual(metadata['boaros_sha256'], harness.digest(base / 'build/run/boaros-kernel'))
            self.assertNotEqual(metadata['boaros_sha256'], harness.digest(kernel))
            self.assertEqual(metadata['status'], 'failed')

    def test_cache_reuse_validates_image_and_config(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            destination = base / 'linux/key'
            image = destination / 'arch/riscv/boot/Image'
            image.parent.mkdir(parents=True)
            image.write_bytes(b'kernel')
            (destination / '.config').write_bytes(b'config')
            inputs = {'source': ['url', 'commit'], 'tools': {}}
            (destination / 'identity.json').write_text(json.dumps({
                'inputs': inputs, 'image_sha256': harness.digest(image),
                'config_sha256': harness.digest(destination / '.config')}))
            with patch.object(harness, 'BUILD', base), patch.object(harness, 'identity', return_value=('key', inputs, 'prefix-')):
                self.assertEqual(harness.linux_build()[0], image)
                image.write_bytes(b'wrong-kernel')
                with self.assertRaisesRegex(RuntimeError, 'integrity failure'):
                    harness.linux_build()

    def test_timeout_and_process_failure_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'raw.log'
            with self.assertRaises(TimeoutError):
                run_logged(['python3', '-c', 'import time; print("started", flush=True); time.sleep(5)'], path, 0.2)
            self.assertIn('started', path.read_text())
            with self.assertRaises(RuntimeError):
                run_logged(['python3', '-c', 'raise SystemExit(3)'], path, 1)

if __name__ == '__main__':
    unittest.main()
