"""Native focus objects must rebuild when their compiler-reported headers change."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativeDependencies(unittest.TestCase):
    def test_wait_and_sync_objects_follow_header_dependencies(self):
        for name in ('wait-native', 'sync-0'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                header = work / 'model.h'
                header.write_text('initial layout')
                compiler = work / 'compiler'
                compiler.write_text('#!' + sys.executable + '\n' + '''import os,sys
from pathlib import Path
argv=sys.argv[1:]
output=Path(argv[argv.index('-o')+1]);output.parent.mkdir(parents=True,exist_ok=True)
output.write_bytes(b'compiled fixture')
with Path(os.environ['COMPILE_LOG']).open('a') as log:log.write('compile\\n')
if '-MMD' in argv:
    output.with_suffix('.d').write_text(str(output)+': include/kernel/cpu.h '+os.environ['MODEL_HEADER']+'\\n')
''')
                compiler.chmod(0o755)
                output = work / (name + '.o')
                env = dict(os.environ, MODEL_HEADER=str(header), COMPILE_LOG=str(work / 'compiled'))
                argv = ['make', '--no-print-directory', 'BUILD_DIR=' + str(work),
                        'CC=' + str(compiler), str(output)]
                subprocess.run(argv, cwd=ROOT, env=env, capture_output=True, check=True)
                self.assertTrue(output.with_suffix('.d').is_file(), 'focus object has no header dependency file')
                header.write_text('changed layout')
                changed = output.stat().st_mtime_ns + 1000000
                os.utime(header, ns=(changed, changed))
                subprocess.run(argv, cwd=ROOT, env=env, capture_output=True, check=True)
                self.assertEqual((work / 'compiled').read_text().splitlines(), ['compile', 'compile'])


if __name__ == '__main__':
    unittest.main()
