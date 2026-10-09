"""The original network image is verified and atomically published."""
import hashlib
import lzma
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
import network_inputs


class RuntimeImage(unittest.TestCase):
    def test_another_owners_image_is_preserved_and_canonical_cache_corruption_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = b'verified release runtime'
            archive = root / 'references/oscomp-images/runtime.xz'
            archive.parent.mkdir(parents=True)
            archive.write_bytes(lzma.compress(data))
            (root / 'references/sources.tsv').write_text('file\toscomp-images/runtime.xz\turl\t-\t' + network_inputs.digest(archive) + '\n')
            config = root / 'tests/workloads/network/inputs.json'
            config.parent.mkdir(parents=True)
            config.write_text(json.dumps({'archive_reference': 'oscomp-images/runtime.xz',
                                         'image_sha256': hashlib.sha256(data).hexdigest()}))
            old = root / 'references/oscomp-autotest/sdcard-rv.img'
            old.parent.mkdir(parents=True)
            old.write_bytes(b'other owner input')
            with patch.object(network_inputs, 'ROOT', root):
                image = network_inputs.prepare()
                self.assertEqual(image.read_bytes(), data)
                self.assertEqual(old.read_bytes(), b'other owner input')
                image.write_bytes(b'corrupt canonical cache')
                with self.assertRaisesRegex(RuntimeError, 'identity mismatch'):
                    network_inputs.prepare()
                self.assertEqual(image.read_bytes(), b'corrupt canonical cache')

    def test_exact_content_sparse_tail_and_verified_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, image = root / 'runtime.xz', root / 'runtime.img'
            data = b'ext4 model\n' + bytes(2 * 1024 * 1024)
            archive.write_bytes(lzma.compress(data))
            archive_sha = network_inputs.digest(archive)
            image_sha = hashlib.sha256(data).hexdigest()
            self.assertEqual(network_inputs.unpack(archive, image, archive_sha, image_sha), image)
            self.assertEqual(image.read_bytes(), data)
            self.assertEqual(network_inputs.unpack(archive, image, archive_sha, image_sha), image)
            image.write_bytes(b'changed cached image')
            with self.assertRaisesRegex(RuntimeError, 'identity mismatch'):
                network_inputs.unpack(archive, image, archive_sha, image_sha)
            self.assertEqual(image.read_bytes(), b'changed cached image')

    def test_invalid_archive_or_raw_identity_never_publishes(self):
        for change in ('archive', 'raw', 'truncated'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                archive, image = root / 'runtime.xz', root / 'runtime.img'
                archive.write_bytes(lzma.compress(b'original runtime'))
                archive_sha = network_inputs.digest(archive)
                if change == 'archive':
                    archive.write_bytes(lzma.compress(b'changed runtime'))
                if change == 'truncated':
                    archive.write_bytes(archive.read_bytes()[:-5])
                    archive_sha = network_inputs.digest(archive)
                with self.assertRaises((RuntimeError, EOFError, lzma.LZMAError)):
                    network_inputs.unpack(archive, image, archive_sha, hashlib.sha256(b'wrong raw').hexdigest())
                self.assertFalse(image.exists())
                self.assertEqual(list(root.glob('.runtime-*')), [])


if __name__ == '__main__':
    unittest.main()
