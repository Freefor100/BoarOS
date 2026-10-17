#!/usr/bin/env python3
"""Run real make rules with a recording compiler; check reverse configuration switches."""
from pathlib import Path
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='boaros-build-config-') as name:
    work = Path(name)
    compiler = work / 'tool-gcc'
    compiler.write_text('''#!/usr/bin/env python3
from pathlib import Path
import json,sys
args=sys.argv[1:]
if '--version' in args: print('recording compiler 1');sys.exit(0)
if '-o' not in args: sys.exit(0)
out=Path(args[args.index('-o')+1]);out.parent.mkdir(parents=True,exist_ok=True)
out.write_text(json.dumps(args))
''')
    compiler.chmod(0o755)
    def make(target, *settings):
        result = subprocess.run(['make', '--no-print-directory', '-j8', str(target),
            f'CROSS_COMPILE={work}/tool-', f'LA_CROSS_COMPILE={work}/tool-',
            f'BUILD_DIR={work}/rv', f'KERNEL_RV={work}/kernel-rv',
            f'LA_BUILD={work}/la', f'KERNEL_LA={work}/kernel-la',
            'INIT_CONFIG=config/init.json', *settings],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        if result.returncode: raise AssertionError(result.stdout)
    def arguments(path): return json.loads(path.read_text())
    kernel = work / 'kernel-rv'
    ext4 = work / 'rv/third_party/lwext4/src/ext4.o'
    for cost in (1, 0, 1):
        make(kernel, f'COST_DIAGNOSTICS={cost}')
        assert f'-DBOAROS_COST_DIAGNOSTICS={cost}' in arguments(ext4), 'lwext4 retained stale diagnostic configuration'
        before = (ext4.stat().st_mtime_ns, kernel.stat().st_mtime_ns)
        make(kernel, f'COST_DIAGNOSTICS={cost}')
        assert (ext4.stat().st_mtime_ns, kernel.stat().st_mtime_ns) == before, 'unchanged lwext4 configuration rebuilt or relinked'
    make(kernel, 'SMP=1', 'USER_SMP=1', 'USER_SMP_COMPOSITION=1')
    assert any('user_smp_composition' in a for a in arguments(kernel))
    make(kernel, 'SMP=1', 'USER_SMP=1', 'USER_SMP_COMPOSITION=0')
    assert not any('user_smp_composition' in a for a in arguments(kernel)), 'removed composition object retained in link'
    common = work / 'rv/net/socket.o'
    assert '-DBOAROS_USER_SMP_COMPOSITION=0' in arguments(common), 'reverse macro switch retained stale object'
    make(kernel, 'ROOT_DRAIN_FIXTURE=1')
    assert any('root_drain_fixture' in a for a in arguments(kernel))
    make(kernel, 'ROOT_DRAIN_FIXTURE=0')
    assert not any('root_drain_fixture' in a for a in arguments(kernel)), 'removed root fixture retained in link'
    assert not any('--wrap=kernel_vfs_unmount' in a for a in arguments(kernel))
    make(kernel, 'CFLAGS_EXTRA=-DBUILD_CONFIGURATION_PROBE=1')
    assert '-DBUILD_CONFIGURATION_PROBE=1' in arguments(common)
    make(kernel)
    assert '-DBUILD_CONFIGURATION_PROBE=1' not in arguments(common), 'removed compiler flag retained'
    before_compile = common.stat().st_mtime_ns
    compiler.write_text(compiler.read_text().replace('recording compiler 1', 'recording compiler 2'))
    make(kernel)
    assert common.stat().st_mtime_ns != before_compile, 'compiler identity change retained stale object'
    ordinary = work / 'kernel-la'
    make(ordinary)
    assert not any('/tests/' in a or '--wrap=' in a for a in arguments(ordinary)), 'ordinary LA kernel contains contract objects or wrappers'
    contract = work / 'la/kernel-contract-la'
    make(contract)
    assert any('/tests/loongarch/' in a for a in arguments(contract))
    assert any('--wrap=physical_page_allocate' in a for a in arguments(contract))
    before = ordinary.stat().st_mtime_ns
    make(ordinary)
    assert ordinary.stat().st_mtime_ns == before, 'unchanged ordinary kernel relinked'
    print('PASS build rules: lwext4 diagnostics 1/0/1 and no-op, reverse composition/root fixture/macros/flags, compiler identity, distinct ordinary/contract LA artifacts, unchanged no-op')
