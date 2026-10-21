"""Pin paused TCG vCPUs and QEMU I/O threads by their QMP-reported identities."""
import json
import os
from pathlib import Path
import socket
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


def pin_qemu_threads(process, qmp_path, host_vcpus, host_io):
    """Identify paused TCG threads through QMP, pin them, then release the guest."""
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
            command('cont')
            result['continued_after_pin']=True
            return result
