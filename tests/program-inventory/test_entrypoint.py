"""The standalone entry point must import its own build environment."""
from pathlib import Path
import subprocess
import sys
import unittest


class Entrypoint(unittest.TestCase):
    def test_local_environment_survives_architecture_import(self):
        directory = Path(__file__).resolve().parent
        code = '''import runpy,sys
sys.path.insert(0,sys.argv[1])
runpy.run_path(sys.argv[1]+"/run.py",run_name="inventory_entry")
import environment
assert callable(environment.build_busybox), environment.__file__
'''
        result = subprocess.run([sys.executable, '-B', '-c', code, str(directory)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
