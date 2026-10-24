"""Pin paused TCG vCPUs and QEMU I/O threads by their QMP-reported identities."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def host_cpu_selection(value, cpus, host_io):
    if value is None:return None
    try:selection=[int(cpu) for cpu in (value.split(',') if isinstance(value,str) else value)]
    except (TypeError,ValueError) as error:raise ValueError('host vCPU list must contain CPU numbers') from error
    if len(selection)!=cpus or len(set(selection+[host_io]))!=cpus+1:
        raise ValueError('host vCPU and I/O CPUs must be distinct, one per guest CPU')
    if not set(selection+[host_io])<=os.sched_getaffinity(0):
        raise ValueError('requested host CPUs are outside the current allowed affinity')
    return selection


def host_cpu_topology(cpus):
    return {str(cpu):{name:(Path('/sys/devices/system/cpu')/f'cpu{cpu}'/'topology'/name).read_text().strip()
        for name in ('physical_package_id','core_id','thread_siblings_list')} for cpu in cpus}


def _riscv_boot_harts(kernel, gdb_socket, gdb):
    # 精确物理入口保留a0；源码行断点可能已越过改写参数的函数序言。
    script = f'''set pagination off
set confirm off
set architecture riscv:rv64
file {json.dumps(str(Path(kernel).resolve(strict=True)))}
target remote {gdb_socket}
python
import gdb, json
reg = lambda name: int(gdb.parse_and_eval('$' + name))
address = lambda name: int(gdb.parse_and_eval('(unsigned long)&' + name))
harts = []
try:
    for thread in gdb.selected_inferior().threads():
        thread.switch()
        pid, remote_id, tid = thread.ptid
        if pid != 1 or tid or remote_id < 1:
            raise RuntimeError('expected one QEMU virt CPU cluster')
        harts.append(dict(cpu_index=remote_id - 1, hart_id=reg('mhartid')))
    offset = address('__kernel_start') - address('__kernel_physical_start')
    entry = address('kernel_main') - offset
    gdb.Breakpoint('*0x%x' % entry, type=gdb.BP_HARDWARE_BREAKPOINT)
    gdb.execute('continue')
    boot = reg('a0')
    if reg('pc') != entry or reg('mhartid') != boot or reg('satp') != 0:
        raise RuntimeError('unexpected BoarOS physical boot entry')
    cpu = gdb.Value(address('boot_cpu') - offset).cast(gdb.lookup_type('struct kernel_cpu').pointer()).dereference()
    if int(cpu['hardware_id']) != boot or int(cpu['initialized']) != 0x43505531:
        raise RuntimeError('BoarOS boot CPU record differs from hart register')
    stop = dict(harts=harts, boot_hart=boot, pc=reg('pc'), kernel_main_physical=entry,
        satp=reg('satp'), boot_cpu_hardware_id=int(cpu['hardware_id']),
        boot_cpu_initialized=int(cpu['initialized']))
finally:
    try:
        for breakpoint in gdb.breakpoints() or ():
            breakpoint.delete()
        remaining = len(gdb.breakpoints() or ())
        if remaining:
            raise RuntimeError('GDB retained a guest breakpoint')
    finally:
        gdb.execute('disconnect')
stop['breakpoints_remaining'] = remaining
print('BOAROS-AFFINITY ' + json.dumps(stop))
end
'''
    with tempfile.NamedTemporaryFile(mode='w', suffix='.gdb') as commands:
        commands.write(script)
        commands.flush()
        result = subprocess.run([str(gdb), '-nx', '-batch', '-x', commands.name],
            text=True, capture_output=True, timeout=30)
    records = [line.removeprefix('BOAROS-AFFINITY ') for line in result.stdout.splitlines()
               if line.startswith('BOAROS-AFFINITY ')]
    if result.returncode or len(records) != 1:
        raise RuntimeError('BoarOS boot affinity GDB failed: ' + result.stdout + result.stderr)
    stop = json.loads(records[0])
    stop.update(gdb_command=result.args, gdb_script=script,
                gdb_stdout=result.stdout, gdb_stderr=result.stderr)
    return stop


def pin_qemu_threads(process, qmp_path, host_vcpus, host_io, *,
                     riscv_kernel=None, gdb_socket=None, gdb='gdb'):
    """Pin paused TCG threads, optionally ordering BoarOS RV64 CPUs by logical ID."""
    if (riscv_kernel is None) != (gdb_socket is None):
        raise ValueError('RISC-V BoarOS kernel and GDB socket must be supplied together')
    deadline=time.monotonic()+10
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(10)
        while True:
            try:connection.connect(str(qmp_path));break
            except (FileNotFoundError,ConnectionRefusedError):
                if process.poll() is not None:raise RuntimeError('QEMU exited before thread pinning')
                if time.monotonic()>=deadline:raise TimeoutError('QEMU QMP socket did not appear')
                time.sleep(.01)
        with connection.makefile('rwb',buffering=0) as stream:
            greeting=json.loads(stream.readline())
            if 'QMP' not in greeting:raise ValueError('missing QMP greeting')
            def command(name):
                stream.write((json.dumps({'execute':name})+'\n').encode())
                while True:
                    line=stream.readline()
                    if not line:raise RuntimeError('QMP disconnected: '+name)
                    reply=json.loads(line)
                    if 'error' in reply:raise RuntimeError('QMP '+name+': '+str(reply['error']))
                    if 'return' in reply:return reply['return']
            command('qmp_capabilities')
            if command('query-status')['running']:raise ValueError('QEMU guest ran before thread pinning')
            cpus=sorted(command('query-cpus-fast'),key=lambda row:row['cpu-index'])
            iothreads=command('query-iothreads')
            if [row['cpu-index'] for row in cpus]!=list(range(len(host_vcpus))):
                raise ValueError('QMP vCPU count differs from affinity request')
            threads={int(entry.name) for entry in (Path('/proc')/str(process.pid)/'task').iterdir()}
            vcpu_tids={row['thread-id'] for row in cpus}
            if len(vcpu_tids)!=len(cpus) or not vcpu_tids<=threads or process.pid in vcpu_tids:
                raise ValueError('QMP did not identify distinct TCG vCPU threads')
            def pin(tid,cpu):
                os.sched_setaffinity(tid,{cpu})
                actual=sorted(os.sched_getaffinity(tid))
                if actual!=[cpu]:raise ValueError('QEMU thread affinity did not apply')
                return {'tid':tid,'host_cpus':actual}
            # QEMU starts with only the I/O CPU allowed, so later main-thread
            # workers inherit the same mask. TCG CPUs receive their own masks.
            other=[pin(tid,host_io) for tid in sorted(threads-vcpu_tids)]
            result={'guest_cpus':len(cpus),'main':next(row for row in other if row['tid']==process.pid),
                    'vcpus':[dict(pin(row['thread-id'],host_vcpus[index]),cpu_index=index)
                              for index,row in enumerate(cpus)],
                    'other_threads':[row for row in other if row['tid']!=process.pid]}
            result['iothreads']=[dict(next(item for item in other if item['tid']==row['thread-id']),id=row['id'])
                                 for row in iothreads]
            result['process_personality']=int((Path('/proc')/str(process.pid)/'personality').read_text(),16)
            if riscv_kernel is not None:
                try:
                    stop = _riscv_boot_harts(riscv_kernel, gdb_socket, gdb)
                    # disconnect不发送detach；后者会在QEMU自动续跑，破坏此处暂停约束。
                    stop['qmp_status_after_disconnect'] = command('query-status')
                    if stop['qmp_status_after_disconnect']['running']:
                        raise ValueError('QEMU resumed before logical CPU pinning')
                    if sorted(command('query-cpus-fast'), key=lambda row: row['cpu-index']) != cpus:
                        raise ValueError('QMP vCPU identities changed during boot affinity discovery')
                    harts = {row['hart_id']: row['cpu_index'] for row in stop['harts']}
                    if (len(stop['harts']) != len(cpus) or len(harts) != len(cpus)
                            or set(harts.values()) != set(range(len(cpus)))
                            or stop['boot_hart'] not in harts or stop['breakpoints_remaining']):
                        raise ValueError('GDB hart identities differ from QMP vCPUs')
                    order = [stop['boot_hart']] + sorted(hart for hart in harts if hart != stop['boot_hart'])
                    logical = {harts[hart]: (index, hart) for index, hart in enumerate(order)}
                    result['vcpus'] = [dict(pin(row['thread-id'], host_vcpus[logical[row['cpu-index']][0]]),
                        cpu_index=row['cpu-index'], logical_id=logical[row['cpu-index']][0],
                        hart_id=logical[row['cpu-index']][1]) for row in cpus]
                    result['boot_stop'] = stop
                    result['paused_after_gdb_disconnect'] = True
                except BaseException:
                    # GDB异常退出或超时可能触发远端detach，先停止再向调用者传播失败。
                    command('stop')
                    if command('query-status')['running']:
                        raise RuntimeError('QEMU still running after boot affinity failure')
                    raise
            command('cont')
            result['continued_after_pin']=True
            return result
