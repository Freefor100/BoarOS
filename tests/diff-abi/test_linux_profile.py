"""Reference capabilities are part of the immutable Linux build identity."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import harness


class LinuxProfileTests(unittest.TestCase):
    def test_explicit_profile_changes_cache_key(self):
        with tempfile.TemporaryDirectory() as temporary:
            first = Path(temporary) / 'first.config'
            second = Path(temporary) / 'second.config'
            first.write_text('CONFIG_NET=n\n')
            second.write_text('CONFIG_NET=y\n')
            with patch.object(harness.shutil, 'which', return_value=str(first)), \
                 patch.object(harness, 'output', return_value='test compiler'):
                key1, data1, _ = harness.identity(first)
                key2, data2, _ = harness.identity(second)
                self.assertNotEqual(key1, key2)
                self.assertNotEqual(data1['config'], data2['config'])


if __name__ == '__main__':
    unittest.main()
