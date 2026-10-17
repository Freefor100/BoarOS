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
    def objects(path): return [arg for arg in arguments(path) if arg.endswith('.o')]
    def check_rv_layout(path):
        linked = objects(path)
        assert len(linked) == len(set(linked)), 'RV production object appears twice'
        c_objects, architecture = [], []
        for obj in linked:
            args = arguments(Path(obj))
            source = args[args.index('-c') + 1]
            if not source.endswith('.c'): continue
            c_objects.append(obj)
            if source.startswith('arch/riscv/') or source == 'kernel/main.c': architecture.append(obj)
        cpu = [str(work / 'rv/kernel/cpu.o'), str(work / 'rv/kernel/raw_lock.o')]
        common = [obj for obj in c_objects if obj not in cpu and obj not in architecture]
        assert c_objects == cpu + architecture + common, 'RV CPU, architecture and common C objects lost their link grouping'
    kernel = work / 'kernel-rv'
    ext4 = work / 'rv/third_party/lwext4/src/ext4.o'
    for cost in (1, 0, 1):
        make(kernel, f'COST_DIAGNOSTICS={cost}')
        assert f'-DBOAROS_COST_DIAGNOSTICS={cost}' in arguments(ext4), 'lwext4 retained stale diagnostic configuration'
        before = (ext4.stat().st_mtime_ns, kernel.stat().st_mtime_ns)
        make(kernel, f'COST_DIAGNOSTICS={cost}')
        assert (ext4.stat().st_mtime_ns, kernel.stat().st_mtime_ns) == before, 'unchanged lwext4 configuration rebuilt or relinked'
    production_objects = objects(kernel)
    check_rv_layout(kernel)
    make(kernel, 'SMP=1', 'USER_SMP=1', 'USER_SMP_COMPOSITION=1')
    assert any('user_smp_composition' in a for a in arguments(kernel))
    assert set(objects(kernel)) == set(production_objects) | {str(work / 'rv/tests/riscv/user_smp_composition.o')}
    make(kernel, 'SMP=1', 'USER_SMP=1', 'USER_SMP_COMPOSITION=0')
    assert not any('user_smp_composition' in a for a in arguments(kernel)), 'removed composition object retained in link'
    assert objects(kernel) == production_objects, 'composition removal changed production objects or order'
    common = work / 'rv/net/socket.o'
    assert '-DBOAROS_USER_SMP_COMPOSITION=0' in arguments(common), 'reverse macro switch retained stale object'
    make(kernel, 'ROOT_DRAIN_FIXTURE=1')
    assert any('root_drain_fixture' in a for a in arguments(kernel))
    assert set(objects(kernel)) == set(production_objects) | {str(work / 'rv/tests/riscv/root_drain_fixture.o')}
    make(kernel, 'ROOT_DRAIN_FIXTURE=0')
    assert not any('root_drain_fixture' in a for a in arguments(kernel)), 'removed root fixture retained in link'
    assert objects(kernel) == production_objects, 'root fixture removal changed production objects or order'
    assert not any('--wrap=kernel_vfs_unmount' in a for a in arguments(kernel))
    make(kernel, 'CFLAGS_EXTRA=-DBUILD_CONFIGURATION_PROBE=1')
    assert '-DBUILD_CONFIGURATION_PROBE=1' in arguments(common)
    make(kernel)
    assert '-DBUILD_CONFIGURATION_PROBE=1' not in arguments(common), 'removed compiler flag retained'
    before_compile = common.stat().st_mtime_ns
    compiler.write_text(compiler.read_text().replace('recording compiler 1', 'recording compiler 2'))
    make(kernel)
    assert common.stat().st_mtime_ns != before_compile, 'compiler identity change retained stale object'
    trap = work / 'rv/tests/kernel-trap-rv'
    make(trap)
    assert any('/tests/riscv/trap_main.o' in a for a in arguments(trap))
    assert any('/fs/files/io.o' in a for a in arguments(trap))
    assert not any(a.endswith(('/kernel/main.o', '/arch/riscv/root_boot.o', '/net/ethernet.o',
                               '/tests/riscv/root_drain_fixture.o')) for a in arguments(trap)), 'RV module test contains root startup or fixture objects'
    excluded = {'arch/riscv/smp.o', 'arch/riscv/root_boot.o', 'kernel/main.o',
                'kernel/boot_memory.o', 'kernel/dtb.o', 'net/ethernet.o', 'arch/riscv/secondary.o'}
    runtime = [obj for obj in production_objects if str(Path(obj).relative_to(work / 'rv')) not in excluded]
    assert objects(trap) == runtime + [str(work / 'rv/tests/riscv/trap_main.o'),
                                       str(work / 'rv/tests/riscv/trap_trigger.o')], 'RV test runtime changed production objects or their order'
    make(trap, 'ROOT_DRAIN_FIXTURE=1')
    assert objects(trap) == runtime + [str(work / 'rv/tests/riscv/trap_main.o'),
                                       str(work / 'rv/tests/riscv/trap_trigger.o')], 'root fixture leaked into module test runtime'
    ordinary = work / 'kernel-la'
    make(ordinary, 'ROOT_DRAIN_FIXTURE=1')
    assert not any('/arch/riscv/' in a for a in arguments(ordinary)), 'ordinary LA kernel contains RV architecture objects'
    assert any('/fs/files/io.o' in a for a in arguments(ordinary))
    assert any('/kernel/sched/process.o' in a for a in arguments(ordinary))
    assert not any('/tests/' in a or '--wrap=' in a for a in arguments(ordinary)), 'ordinary LA kernel contains contract objects or wrappers'
    contract = work / 'la/kernel-contract-la'
    make(contract)
    assert any('/tests/loongarch/' in a for a in arguments(contract))
    assert any('--wrap=physical_page_allocate' in a for a in arguments(contract))
    before = ordinary.stat().st_mtime_ns
    make(ordinary)
    assert ordinary.stat().st_mtime_ns == before, 'unchanged ordinary kernel relinked'
    print('PASS build rules: lwext4 diagnostics 1/0/1 and no-op, reverse composition/root fixture/macros/flags, compiler identity, distinct ordinary/contract LA artifacts, unchanged no-op')
