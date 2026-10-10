"""CI consumers must start installed tools and honor Make's compiler choice."""
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ci_command_tools', ROOT / 'tests/ci/tools.py')
tools = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tools)


class NativeCommands(unittest.TestCase):
    def tool_cache(self, root, arch, failure=None):
        profile = root / 'tests/userland/glibc' / ('inputs.json' if arch == 'riscv' else 'inputs-loongarch.json')
        profile.parent.mkdir(parents=True)
        profile.write_bytes(b'fixed profile')
        cache = root / 'build/tools' / arch
        binary = cache / ('usr/bin' if arch == 'riscv' else 'bin')
        binary.mkdir(parents=True)
        prefix = 'riscv64-linux-gnu-' if arch == 'riscv' else 'loongarch64-unknown-linux-gnu-'
        for tool in ('gcc', 'as', 'ld', 'ar', 'objdump', 'readelf'):
            path = binary / (prefix + tool)
            if tool == failure:
                body = 'echo "error while loading shared libraries: libdebuginfod.so.1" >&2\nexit 127\n'
            else:
                body = 'printf "%s\\n" "$0" >> "$COMMAND_LOG"\nprintf "tool version\\n"\n'
            path.write_text('#!/bin/sh\n' + body)
            path.chmod(0o755)
        sources = [['file', name, 'fixed-url', '-', 'fixed-sha'] for name in tools.PACKAGES[arch]]
        inputs = {'arch': arch, 'archives': {row[1]: row[4] for row in sources},
                  'profile': tools.digest(profile)}
        (cache / 'ci-identity.json').write_text(json.dumps({
            'inputs': inputs, 'installation': tools.installation(cache)}))
        return cache, sources

    def test_valid_cache_rejects_a_tool_that_cannot_start(self):
        for arch in ('riscv', 'loongarch'):
            for failed in ('gcc', 'readelf'):
                with self.subTest(arch=arch, failed=failed), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    _, sources = self.tool_cache(root, arch, failed)
                    with patch.object(tools, 'ROOT', root), patch.object(tools, 'rows', return_value=sources), \
                            patch.object(tools, 'checked_inputs'), patch.dict(os.environ, COMMAND_LOG=str(root / 'started')):
                        with self.assertRaisesRegex(RuntimeError, '127.*libdebuginfod|libdebuginfod.*127'):
                            tools.prepare_tools(arch)

    def test_verified_cache_starts_all_consumer_tools(self):
        for arch in ('riscv', 'loongarch'):
            with self.subTest(arch=arch), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                cache, sources = self.tool_cache(root, arch)
                log = root / 'started'
                with patch.object(tools, 'ROOT', root), patch.object(tools, 'rows', return_value=sources), \
                        patch.object(tools, 'checked_inputs'), patch.dict(os.environ, COMMAND_LOG=str(log)):
                    self.assertEqual(tools.prepare_tools(arch), cache)
                self.assertTrue(log.is_file(), 'verified cache never starts its tools')
                self.assertEqual({Path(row).name.rsplit('-', 1)[-1] for row in log.read_text().splitlines()},
                                 {'gcc', 'as', 'ld', 'ar', 'objdump', 'readelf'})

    def test_sqlite_make_compiler_reaches_the_shared_runner(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory)
            interpreter = binary / 'python3'
            interpreter.write_text('#!/bin/sh\nexec "$HOST_PYTHON" -c '
                + shlex.quote('import json,sys;print(json.dumps(sys.argv[1:]))') + ' "$@"\n')
            interpreter.chmod(0o755)
            compiler = str(binary / 'custom-target-gcc')
            command = ['make', '-n', 'INIT_CONFIG=config/init.json',
                       'CROSS_COMPILE=' + str(binary / 'custom-target-')]
            # 仅验证实际recipe的选择传递，不使host门禁依赖客体archive或build缓存。
            for name in ('SQLITE_ROLLBACK_RV', 'SQLITE_CLI_STATIC_RV', 'SQLITE_CLI_DYNAMIC_RV',
                         'SQLITE_CLI_INIT_RV', 'MUSL_LDSO', 'KERNEL_RV'):
                dependency = binary / name.lower()
                dependency.write_bytes(b'fixture input')
                command += [name + '=' + str(dependency), '-o', str(dependency)]
            result = subprocess.run([*command, 'test-sqlite-rollback-riscv'],
                cwd=ROOT, capture_output=True, text=True, check=True)
            lines = result.stdout.replace('\\\n', '').splitlines()
            recipe = next(line for line in lines if './tests/sqlite-rollback-riscv.sh' in line)
            env = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ['PATH'], HOST_PYTHON=sys.executable)
            observed = subprocess.run(['sh', '-c', recipe], cwd=ROOT, env=env,
                                      capture_output=True, text=True, check=True)
            argv = json.loads(observed.stdout)
            self.assertIn('--cc', argv, 'Make compiler selection is lost at the shell wrapper')
            self.assertEqual(argv[argv.index('--cc') + 1], compiler)
            self.assertEqual(argv[argv.index('--arch') + 1], 'riscv')


if __name__ == '__main__':
    unittest.main()
