"""The standalone entry point must import its own build environment."""
from pathlib import Path
import subprocess
import sys
import unittest
import tempfile


class Entrypoint(unittest.TestCase):
    def test_user_profile_preserves_nonempty_explicit_output(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence=Path(directory)/'inventory.json';evidence.write_text('original frozen evidence')
            result=subprocess.run([sys.executable,'-B',str(Path(__file__).with_name('run.py')),
                '--execution-profile','rv-user-smp','--firmware','/not-needed-for-validation',
                '--output',directory],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('nonempty',result.stderr)
            self.assertEqual(evidence.read_text(),'original frozen evidence')
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
