"""Verify effective bootstrap selection through real make dry runs."""
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def selected_configs(command, environment):
    result = subprocess.run([command[0], '-n', '--no-print-directory', *command[1:]],
                            cwd=ROOT, env=environment, capture_output=True, text=True, check=True)
    selected = []
    for line in result.stdout.splitlines():
        if 'tools/init-config.py' not in line: continue
        words = shlex.split(line)
        if 'tools/init-config.py' in words:
            selected.append(words[words.index('tools/init-config.py') + 1])
    return selected


class BuildTests(unittest.TestCase):
    def test_common_config_overrides_command_line_and_environment_arch_configs(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = [Path(temporary) / (name + '.json') for name in ('common', 'rv', 'la')]
            for path in paths:
                path.write_text(json.dumps({'path': '/init', 'argv': ['/init'], 'envp': []}))
            environment = dict(os.environ)
            for name in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES', 'INIT_CONFIG', 'INIT_CONFIG_RV', 'INIT_CONFIG_LA'):
                environment.pop(name, None)
            environment.update(INIT_CONFIG_RV=str(paths[1]), INIT_CONFIG_LA=str(paths[2]))
            for extras in ([], ['INIT_CONFIG_RV=' + str(paths[1])],
                           ['INIT_CONFIG_RV=' + str(paths[1]), 'INIT_CONFIG_LA=' + str(paths[2])]):
                with self.subTest(extras=extras):
                    selected = selected_configs(['make', 'all', 'INIT_CONFIG=' + str(paths[0]), *extras], environment)
                    self.assertEqual(selected, [str(paths[0]), str(paths[0])])

    def test_diagnostic_uses_recorded_configs_despite_inherited_make_overrides(self):
        spec = importlib.util.spec_from_file_location('diagnostic_build_runner', HERE / 'run.py')
        runner = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)
        with patch.dict(os.environ, {'INIT_CONFIG': 'config/init.json',
                                    'INIT_CONFIG_RV': 'config/init.json',
                                    'INIT_CONFIG_LA': 'config/init.json',
                                    'MAKEFLAGS': '-- INIT_CONFIG=config/init.json'}):
            command, environment, configs = runner.diagnostic_build(('riscv', 'loongarch'), ['basic'], 17, [])
        self.assertEqual(set(configs), {'riscv', 'loongarch'})
        self.assertEqual(set(selected_configs(command, environment)), set(map(str, configs.values())))
        self.assertNotIn('MAKEFLAGS', environment)
        self.assertNotIn('INIT_CONFIG', environment)


if __name__ == '__main__':
    unittest.main()
