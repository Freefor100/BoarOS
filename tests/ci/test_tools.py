"""Cached installations must preserve contents, permissions and links."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('ci_tools', Path(__file__).with_name('tools.py'))
tools = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tools)


class Identity(unittest.TestCase):
    def test_producer_input_changes_invalidate_outer_cache(self):
        changed_inputs = ('.github/actions/native-environment/action.yml', 'tests/program-inventory/inputs.json',
                          'tests/diff-abi/harness.py', 'tests/diff-abi/linux.config')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in set(tools.CACHE_INPUTS) | set(changed_inputs):
                file = root / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes((tools.ROOT / name).read_bytes())
            model = root / 'host-tool'
            model.write_bytes(b'fixed host tool')
            with patch.object(tools, 'ROOT', root), patch.object(tools.shutil, 'which', return_value=str(model)):
                for arch in ('riscv', 'loongarch'):
                    for name in changed_inputs:
                        with self.subTest(arch=arch, changed=name):
                            before = tools.cache_key(arch)
                            file = root / name
                            original = file.read_bytes()
                            file.write_bytes(original + b'\n')
                            self.assertNotEqual(before, tools.cache_key(arch))
                            file.write_bytes(original)

    def test_content_permission_link_and_missing_file_change_identity(self):
        for change in ('content', 'mode', 'link', 'missing'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                file = root / 'crt.o'
                file.write_bytes(b'original CRT')
                (root / 'loader').symlink_to('crt.o')
                before = tools.installation(root)
                (root / 'ci-identity.json').write_text('stamp')
                self.assertEqual(before, tools.installation(root))
                if change == 'content':
                    file.write_bytes(b'changed CRT')
                elif change == 'mode':
                    file.chmod(file.stat().st_mode ^ 0o100)
                elif change == 'link':
                    (root / 'loader').unlink()
                    (root / 'loader').symlink_to('different')
                else:
                    file.unlink()
                self.assertNotEqual(before, tools.installation(root))


if __name__ == '__main__':
    unittest.main()
