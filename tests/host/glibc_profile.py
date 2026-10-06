#!/usr/bin/env python3
"""Pinned runtime identity includes permissions and link targets, not mtime."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('profile',ROOT/'tests/userland/glibc/profiles.py')
profile=importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)

class Installation(unittest.TestCase):
    def test_each_identity_change(self):
        for change in ('content','mode','link','missing'):
            with self.subTest(change=change),tempfile.TemporaryDirectory() as temp:
                root=Path(temp);(root/'header').write_text('original');(root/'link').symlink_to('header')
                expected=profile.tree_digest(root)
                self.assertEqual(profile.tree_digest(root),expected)
                if change=='content':(root/'header').write_text('modified')
                elif change=='mode':(root/'header').chmod((root/'header').stat().st_mode ^ 0o100)
                elif change=='link':(root/'link').unlink();(root/'link').symlink_to('other')
                else:(root/'header').unlink()
                self.assertNotEqual(profile.tree_digest(root),expected)

if __name__=='__main__':unittest.main()
