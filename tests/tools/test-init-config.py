#!/usr/bin/env python3
"""Initial-process strings use the same default exec budget as user exec."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('init_config', ROOT / 'tools/init-config.py')
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)


class ExecLimits(unittest.TestCase):
    def test_total_budget(self):
        # 32 strings, 32 pointers and the separately copied /init name.
        available = 2 * 1024 * 1024 - 32 * 8 - len('/init') - 1
        strings = ['x' * (available // 32 - 1)] * 32
        strings[0] += 'x' * (available % 32)
        value = dict(path='/init', argv=strings, envp=[])
        config.render(value)
        strings[0] += 'x'
        with self.assertRaises(ValueError):
            config.render(value)

    def test_architecture_string_limit(self):
        for page in (4096, 16384):
            value = dict(path='/init', argv=['x' * (page * 32 - 1)], envp=[])
            config.render(value, page)
            value['argv'][0] += 'x'
            with self.assertRaises(ValueError):
                config.render(value, page)

    def test_pointer_space(self):
        with self.assertRaises(ValueError):
            config.render(dict(path='/init', argv=[''] * 240000, envp=[]))


if __name__ == '__main__':
    unittest.main()
