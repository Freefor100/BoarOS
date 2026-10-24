#!/usr/bin/env python3
"""Real U-mode network contracts against the fixed Linux reference."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests/diff-abi'))
import harness
from arch_profiles import PROFILES

def workload_source(name):
    return ROOT / ({'memory':'tests/workloads/resource/memory.c','mremap':'tests/workloads/mremap/caller.c','futex':'tests/workloads/futex/caller.c','posix':'tests/workloads/signal/posix.c','queued':'tests/workloads/signal/queued.c','altstack':'tests/workloads/signal/altstack.c','event':'tests/workloads/event/caller.c','pipe':'tests/workloads/pipe/caller.c','epoll':'tests/workloads/epoll/delivery.c'}.get(name,f'tests/workloads/network/{name}.c'))

def futex_lines(raw,cpus):
    names=['abi','atomics']+[f'compare-{cmp}-old-{value}' for cmp in range(6) for value in (-1,0,1)]+['requeue-near','requeue-offset','wake-zero','wake-negative','wake-both','shared','robust']
    expected=[f'FUTEX CALLER CPUS {cpus}',*[f'FUTEX CASE {name} PASS' for name in names],*[f'FUTEX CPU {cpu} PASS' for cpu in range(cpus)],'FUTEX CASE caller-cpus PASS','FUTEX CALLER PASS cases=28']
    lines=[line[line.index('FUTEX '):] for line in raw.replace('\r','').splitlines() if any(token in line for token in ('FUTEX CALLER ','FUTEX CASE ','FUTEX CPU '))]
    return lines,expected

def mremap_output(raw,cpus):
    lines=raw.replace('\r','').splitlines()
    actual=[line for line in lines if line.startswith(('MREMAP CASE ','MREMAP END '))]
    expected=[f'MREMAP CASE {name} PASS' for name in ('realloc','remote-move-reuse','shared-file-msync-truncate','sysv-shrink-ownership')]+['MREMAP END 4']
    remote=re.findall(r'^MREMAP CPU remote index=(\d+) affinity=(\d+) role=(?:mover|reader) content=PASS$',raw.replace('\r',''),re.M)
    return (actual==expected and [(int(i),int(a)) for i,a in remote]==[(i,i) for i in range(cpus)] and
            len([line for line in lines if line.startswith(f'MREMAP BEGIN cpus={cpus} ')])==1 and
            len([line for line in lines if line.startswith('MREMAP METRIC realloc ')])==1 and
            not any(line.startswith('MREMAP FAIL') for line in lines))


def unix_named_output(raw,performance):
    lines=[line for line in raw.replace('\r','').splitlines() if line.startswith('UNIX-NAMED ')]
    if not performance:
        names=('stream-names-accept4-shutdown','inode-rename-hardlink-unlink','dgram-reconnect-unspec-source-autobind','credentials-snapshots-dac','fault-no-sideeffects','queue-space-poll-epoll-backlog','many-senders-source-order')
        return lines==[*(f'UNIX-NAMED CASE {name} PASS' for name in names),'UNIX-NAMED END 7']
    metrics=re.findall(r'^UNIX-NAMED PERF name=(\S+) bytes=(\d+) count=(\d+) wall_ns=(\d+) bytes_per_sec=(\d+) ops_per_sec=(\d+) p50_ns=(\d+) p95_ns=(\d+) max_ns=(\d+) thread_cpu_ns=(\d+) rss_kib=(\d+)$',raw.replace('\r',''),re.M)
    expected=[('pathname-stream-four',16777216,256),('abstract-dgram-four',524288,4096),('pathname-stream-connections',0,1024)]
    return len(lines)==4 and lines[-1]=='UNIX-NAMED PERF END 3' and len(metrics)==3 and all(
        (row[0],int(row[1]),int(row[2]))==want and int(row[3])>0 and int(row[5])>0 and
        0<int(row[6])<=int(row[7])<=int(row[8]) and int(row[9])>0 and int(row[10])>=0
        for row,want in zip(metrics,expected))


def unix_control_output(raw,performance,cpus):
    lines=[line for line in raw.replace('\r','').splitlines() if 'UNIX-CONTROL ' in line]
    if not lines or any(not line.startswith('UNIX-CONTROL ') for line in lines):return False
    resource=re.fullmatch(r'UNIX-CONTROL RESOURCE fd_before=(\d+) fd_after=(\d+)',lines[-1])
    if not resource or resource[1]!=resource[2]:return False
    if not performance:
        names=('rights-file-pipe-socket-eventfd-opath-shared-state-cloexec',
            'send-retains-file-after-close-and-fd-reuse','control-none-small-multiple-records-truncation',
            'credentials-partial-control-and-rights-truncation','bad-fd-and-253-254-rights-limit',
            'receive-full-fd-table-payload-and-partial-rights',
            'inflight-nofile-eight-ninth-allowed-tenth-rejected-drain-uid-account',
            'exit-group-blocked-sendmsg-releases-unpublished-rights','peek-duplicates-and-plain-read-discards-rights',
            'stream-rights-and-credential-boundaries','stream-zero-peek-read-fault-null-cookie-old-packet',
            'empty-datagram-delivers-rights-and-credentials',
            'credentials-real-ids-peer-snapshot-explicit-permissions','passcred-option-autobind-listener-inheritance',
            'fault-send-control-no-enqueue','libc-sendmsg-control-fault-observation',
            'null-control-nonzero-capacity-discards-rights','null-name-skips-readonly-namelen-output',
            'fault-receive-control-returns-payload-ctrunc',
            'fault-partial-control-installs-first-fd','fault-dgram-payload-consumes-and-drops-rights',
            'malformed-and-empty-rights-control','concurrent-transfer-source-close-with-cycle-churn')
        libc=[line for line in lines if line.startswith('UNIX-CONTROL ACTION libc-sendmsg-inaccessible-control ')]
        if len(libc)!=1 or libc[0] not in ('UNIX-CONTROL ACTION libc-sendmsg-inaccessible-control outcome=sigsegv',
            'UNIX-CONTROL ACTION libc-sendmsg-inaccessible-control outcome=efault'):return False
        actions={
            'inflight-nofile-eight-ninth-allowed-tenth-rejected-drain-uid-account':'UNIX-CONTROL ACTION inflight-quota-numeric-uid-rotation executed=1',
            'exit-group-blocked-sendmsg-releases-unpublished-rights':'UNIX-CONTROL ACTION exit-group-pending-control sleeping_sender=1 original_pipe_writer_closed=1 eof_after_wait=1',
            'stream-zero-peek-read-fault-null-cookie-old-packet':'UNIX-CONTROL ACTION stream-null-cookie pid=0 uid=0 gid=0',
            'credentials-real-ids-peer-snapshot-explicit-permissions':'UNIX-CONTROL ACTION credentials-distinct-real-effective-ids executed=1',
            'libc-sendmsg-control-fault-observation':libc[0]}
        expected=[]
        for name in names:
            if name in actions:expected.append(actions[name])
            expected.append(f'UNIX-CONTROL CASE {name} PASS')
        expected += ['UNIX-CONTROL ACTION gc-concurrent-churn queues=32 closed_user_fds=64',
            'UNIX-CONTROL ACTION gc-self-cycle queues=32 closed_user_fds=64',
            'UNIX-CONTROL ACTION gc-mutual-cycle queues=64 closed_user_fds=128',
            'UNIX-CONTROL ACTION gc-external-root retained_message_read=1 closed_user_fds=4',
            'UNIX-CONTROL ACTION gc-unaccepted-child queued_children=16 closed_user_fds=32',
            'UNIX-CONTROL END 23']
        return lines[:-1]==expected
    metrics=[re.fullmatch(r'UNIX-CONTROL PERF name=(stream|dgram)-(none|passcred|rights) type=(stream|dgram) mode=(none|passcred|rights) pairs=4 rounds_per_pair=1024 count=4096 bytes=524288 cpus=(1|2|4) wall_ns=(\d+) thread_cpu_ns=(\d+) p50_ns=(\d+) p95_ns=(\d+) max_ns=(\d+) ops_per_sec=(\d+)',line) for line in lines[:-2]]
    expected=[(kind,mode) for kind in ('stream','dgram') for mode in ('none','passcred','rights')]
    return (len(lines)==8 and lines[-2]=='UNIX-CONTROL PERF END 6' and len(metrics)==6 and
        all(row and (row[1],row[2])==want and (row[3],row[4])==want and int(row[5])==cpus and
            int(row[6])>0 and int(row[7])>0 and 0<int(row[8])<=int(row[9])<=int(row[10]) and int(row[11])>0
            for row,want in zip(metrics,expected)))


def unix_seqpacket_owners(raw,arch):
    if not PROFILES[arch].root_success(raw,0):return False
    if arch=='loongarch':return 'LA root owners released; pages=' in raw and ' heap-live=0' in raw
    stacks=re.findall(r'BoarOS: task stacks released=(0x[0-9a-f]+) min-free=(0x[0-9a-f]+) max-used=(0x[0-9a-f]+)',raw)
    return len(stacks)==1 and int(stacks[0][0],16)>0 and int(stacks[0][1],16)>=1024 and int(stacks[0][2],16)>0


def unix_seqpacket_output(raw,performance,cpus):
    lines=[line for line in raw.replace('\r','').splitlines() if any(prefix in line for prefix in ('UNIXSEQ ','UNIX-CONTROL '))]
    if not lines or any(not line.startswith(('UNIXSEQ ','UNIX-CONTROL ')) for line in lines):return False
    resource=re.fullmatch(r'UNIXSEQ RESOURCE fd_before=(\d+) fd_after=(\d+)',lines[-1])
    if not resource or resource[1]!=resource[2]:return False
    if performance:
        metrics=[re.fullmatch(r'UNIXSEQ PERF name=seqpacket-(none|passcred|rights) type=seqpacket mode=(none|passcred|rights) pairs=4 rounds_per_pair=1024 count=4096 bytes=524288 cpus=(1|2|4) wall_ns=(\d+) thread_cpu_ns=(\d+) p50_ns=(\d+) p95_ns=(\d+) max_ns=(\d+) ops_per_sec=(\d+)',line) for line in lines[:-2]]
        return (len(lines)==5 and lines[-2]=='UNIXSEQ PERF END 3' and len(metrics)==3 and
            all(row and row[1]==row[2]==mode and int(row[3])==cpus and int(row[4])>0 and int(row[5])>0 and
                0<int(row[6])<=int(row[7])<=int(row[8]) and int(row[9])>0
                for row,mode in zip(metrics,('none','passcred','rights'))))
    focus=('socketpair-type-flags-path-abstract-autobind-peercred',
        'unconnected-listener-flags-more-eor-waitall-sendto',
        'record-peek-trunc-empty-zero-read-recv-eof-controls',
        'vectored-content-fault-atomic-send-consumed-receive-size-limit',
        'raw-header-iov-control-import-before-state-reset-and-fd-interpretation',
        'final-msghdr-fault-payload-consumed-installed-right-survives',
        'source-current-name-after-send-latebind-and-sender-close',
        'shutdown-drain-halfclose-reset-empty-child-no-sigpipe',
        'nonblock-backlog-accept-invalid-flags-address-fault-consumes-child',
        'poll-epoll-level-edge-oneshot-blocked-wake-eof')
    expected=[]
    for name in focus:
        if name=='shutdown-drain-halfclose-reset-empty-child-no-sigpipe':expected.append('UNIXSEQ ACTION sigpipe send=0 write=0 sendfile=0 total=0')
        if name=='poll-epoll-level-edge-oneshot-blocked-wake-eof':expected += [f'UNIXSEQ CPU index={cpu} affinity={cpu} rights=1 PASS' for cpu in range(cpus)]
        expected.append(f'UNIXSEQ CASE {name} PASS')
    control=('rights-file-pipe-socket-eventfd-opath-shared-state-cloexec',
        'send-retains-file-after-close-and-fd-reuse','control-none-small-multiple-records-truncation',
        'credentials-partial-control-and-rights-truncation','bad-fd-and-253-254-rights-limit',
        'receive-full-fd-table-payload-and-partial-rights',
        'inflight-nofile-eight-ninth-allowed-tenth-rejected-drain-uid-account',
        'exit-group-blocked-sendmsg-releases-unpublished-rights','peek-duplicates-and-plain-read-discards-rights',
        'credentials-real-ids-peer-snapshot-explicit-permissions','fault-send-control-no-enqueue',
        'libc-sendmsg-control-fault-observation','null-control-nonzero-capacity-discards-rights',
        'null-name-skips-readonly-namelen-output','fault-receive-control-returns-payload-ctrunc',
        'fault-partial-control-installs-first-fd','fault-dgram-payload-consumes-and-drops-rights',
        'malformed-and-empty-rights-control','concurrent-transfer-source-close-with-cycle-churn')
    libc=[line for line in lines if line.startswith('UNIX-CONTROL ACTION libc-sendmsg-inaccessible-control ')]
    if len(libc)!=1 or libc[0] not in ('UNIX-CONTROL ACTION libc-sendmsg-inaccessible-control outcome=sigsegv',
        'UNIX-CONTROL ACTION libc-sendmsg-inaccessible-control outcome=efault'):return False
    actions={
        'inflight-nofile-eight-ninth-allowed-tenth-rejected-drain-uid-account':'UNIX-CONTROL ACTION inflight-quota-numeric-uid-rotation executed=1',
        'exit-group-blocked-sendmsg-releases-unpublished-rights':'UNIX-CONTROL ACTION exit-group-pending-control sleeping_sender=1 original_pipe_writer_closed=1 eof_after_wait=1',
        'credentials-real-ids-peer-snapshot-explicit-permissions':'UNIX-CONTROL ACTION credentials-distinct-real-effective-ids executed=1',
        'libc-sendmsg-control-fault-observation':libc[0]}
    for name in control:
        if name in actions:expected.append(actions[name])
        expected.append(f'UNIX-CONTROL CASE {name} PASS')
    expected += ['UNIX-CONTROL ACTION gc-concurrent-churn queues=32 closed_user_fds=64',
        'UNIX-CONTROL ACTION gc-self-cycle queues=32 closed_user_fds=64',
        'UNIX-CONTROL ACTION gc-mutual-cycle queues=64 closed_user_fds=128',
        'UNIX-CONTROL ACTION gc-external-root retained_message_read=1 closed_user_fds=4',
        'UNIX-CONTROL ACTION gc-unaccepted-child queued_children=16 closed_user_fds=32',
        'UNIXSEQ END focus=10 control=19']
    return lines[:-1]==expected


# Exact, ordered protocol: 30 semaphore cases, 4 independent socketpair cases,
# one real affinity/commit/wait/undo row per CPU, and unchanged FD/IPC counts.
SEM_CASES=('native64-libc-raw','private-key-create-exclusive','stat-set-info-index',
    'values-all-pid-times','atomic-duplicate-indices','atomic-rollback-nowait',
    'operation-index-value-ranges','wait-zero-count-wake','wait-negative-count-wake',
    'timeout-validation-no-mutation','signal-eintr-waiter-removed','remove-wakes-both-waiters',
    'numeric-ids-permissions','fault-import-no-sideeffects','undo-normal-exit',
    'undo-fork-no-inheritance','undo-pthread-share-before-first','undo-clone-share-before-first',
    'undo-last-shared-exit','undo-exec-preserve','undo-nonleader-exec-preserve',
    'undo-setval-clears-one','undo-setall-clears-all','undo-exit-clamps-bounds',
    'undo-adjustment-range-rollback','clone-invalid-flags-owner-retained',
    'published-limits-large-user-import','remove-discards-stale-undo',
    'zero-operation-read-permission','actual-caller-cpus')

def sysv_sem_output(raw,cpus):
    lines=[line for line in raw.replace('\r','').splitlines() if 'SYSV-SEM ' in line or 'SOCKETPAIR ' in line]
    expected=[f'SYSV-SEM BEGIN cpus={cpus}']
    for name in SEM_CASES:
        if name=='actual-caller-cpus':expected += [f'SYSV-SEM CPU index={cpu} affinity={cpu} commit=1 wait=1 undo=1 PASS' for cpu in range(cpus)]
        expected.append(f'SYSV-SEM CASE {name} PASS')
    expected += ['SYSV-SEM END 30',*(f'SOCKETPAIR CASE {name} PASS' for name in (
        'creation-errors-output-prefix','fault-before-create-no-installed-fd',
        'flags-and-live-unix-content','fd-reservation-before-family')),'SOCKETPAIR END 4']
    if len(lines)!=len(expected)+1 or lines[:-1]!=expected:return False
    resource=re.fullmatch(r'SYSV-SEM RESOURCE fd_before=(\d+) fd_after=(\d+) sets_before=(\d+) sets_after=(\d+) members_before=(\d+) members_after=(\d+)',lines[-1])
    return bool(resource and all(resource[i]==resource[i+1] for i in (1,3,5)))


MSG_CASES=('native64-libc-raw','key-generation-exclusive','fifo-equal-except-negative-long-min',
    'e2big-preserves-noerror-consumes','byte-and-zero-message-count-capacity',
    'stat-info-index-native-command','copy-ordinal-errors-and-no-mutation',
    'send-import-copy-prepare-receive-consumed-fault','numeric-uid-permissions-and-stat-any','fork-blocking-bidirectional-content',
    'signal-no-restart','actual-caller-cpus')
def sysv_msg_output(raw,cpus):
    lines=[line for line in raw.replace('\r','').splitlines() if line.startswith('SYSV-MSG ')]
    expected=['SYSV-MSG BEGIN']
    for name in MSG_CASES:
        if name=='actual-caller-cpus':expected += [f'SYSV-MSG CPU index={cpu} affinity={cpu} send=1 receive=1 PASS' for cpu in range(cpus)]
        expected.append(f'SYSV-MSG CASE {name} PASS')
    return lines==expected+['SYSV-MSG RESOURCE queues=0 messages=0 bytes=0','SYSV-MSG END 12']

MEMORY_CASES=('as-map','as-fixed','data-private','data-zero','data-fixed','protect-prefix',
    'protect-hole','remap-growth','remap-fixed','brk-same-page','shm-as','stack-initial',
    'stack-as','stack-soft','file-shm-data','caller-limits','shared-admission','limit-abi','limit-thread-fork',
    'limit-exec','limit-nonleader-exec')
def memory_output(raw):
    lines=[line for line in raw.replace('\r','').splitlines() if line.startswith('MEMORY ')]
    cases=[re.fullmatch(r'MEMORY CASE (\S+) PASS checks=([1-9][0-9]*)',line) for line in lines[:-1]]
    return (len(cases)==len(MEMORY_CASES) and all(cases) and
        tuple(case[1] for case in cases)==MEMORY_CASES and lines[-1]=='MEMORY END cases=21 failures=0')

def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()

def run_loongarch(args):
    import tempfile
    from arch_profiles import PROFILES
    profile=PROFILES['loongarch']
    sys.path.insert(0,str(ROOT/'tests/loongarch'))
    from guest import run_guest
    from root import disk,execute
    from reference import archive
    work=args.output or Path(tempfile.mkdtemp(prefix='network-run.',dir=ROOT/'build/loongarch'))
    if args.output:work.mkdir(parents=True,exist_ok=False)
    program=work/'init'
    environment=os.environ.copy();environment['REALGCC']=str(ROOT/'build/loongarch/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc')
    compiler=ROOT/'build/loongarch/musl-root/bin/musl-gcc'
    subprocess.run([str(compiler),*profile.raw_flags,*(['-DPIPE_SMOKE=1'] if args.workload=='pipe' else ['-DEVENT_SMOKE=1'] if args.workload=='event' else []),'-static','-O2','-pthread','-Wall','-Wextra','-Werror',
        '-Wl,-z,max-page-size=16384',str(workload_source(args.workload)),'-o',str(program)],env=environment,check=True)
    supervisor=work/'supervisor'
    execute([profile.compiler,*profile.raw_flags,'-O2','-DROOT_NETWORK_SETUP=1','-ffreestanding',
        '-fno-builtin','-fno-stack-protector','-nostdlib','-nostartfiles','-static','-no-pie',
        '-Wl,--build-id=none','-Wl,-z,max-page-size=16384','-T','tests/common/user.ld',
        'tests/loongarch/root_linux_init.c','tests/common/user_start.S','-o',supervisor])
    initrd=work/'initramfs.gz';initrd.write_bytes(archive([('dev',0o040755,b'',0,0),('dev/console',0o020600,b'',5,1),
        ('init',0o100755,supervisor.read_bytes(),0,0),('TRAILER!!!',0,b'',0,0)]))
    metadata={'arch':profile.name,'program_sha256':digest(program),'runs':{}}
    qemu=os.environ.get('QEMU_LOONGARCH64',profile.qemu)
    for platform in ('linux','boaros'):
        if args.only and args.only!=platform:continue
        for memory in args.memory or ('512M','1G'):
            case=work/f'{platform}-{memory}';case.mkdir()
            image=harness.fixture(case,program) if args.workload in ('pipe','event','altstack','queued','posix','futex','mremap','unix_control','unix_seqpacket','sysv_sem','sysv_msg','memory') else disk(case,program)
            kernel=profile.linux_kernel() if platform=='linux' else args.kernel or ROOT/profile.kernel
            snapshot=case/'kernel';shutil.copyfile(kernel,snapshot)
            command=profile.boot(qemu,snapshot,memory)+['-drive',f'file={image},if=none,format=raw,id=root',
                '-device',profile.block('modern'),'-net','none']
            if args.platform_config=='official':
                command=command[:-2]+['-device','virtio-net-pci,netdev=net,addr=5,disable-legacy=on','-netdev','user,id=net','-rtc','base=utc']
            if platform=='linux':command+=['-initrd',str(initrd),'-append','console=ttyS0 rdinit=/init loglevel=3']
            try:code,raw=run_guest(command,120)
            except subprocess.TimeoutExpired as error:code=-1;raw=error.output
            (case/'console.log').write_text(raw)
            passed=not code and ('FUTEX CALLER PASS cases=28' if args.workload=='futex' else 'POSIX END failures=0' if args.workload=='posix' else 'QUEUED END failures=0' if args.workload=='queued' else 'ALTSTACK END 6' if args.workload=='altstack' else 'EPOLL PASS delivery' if args.workload=='epoll' else 'PIPE END 7' if args.workload=='pipe' else 'EVENT END 8' if args.workload=='event' else f'NETWORK PASS {args.workload}') in raw and 'fatal' not in raw and (
                'Linux LA root application passed' in raw if platform=='linux' else profile.root_success(raw,0))
            if digest(program)!=metadata['program_sha256']:raise RuntimeError('ELF changed during comparison')
            if args.workload=='futex':
                lines,expected=futex_lines(raw,1);passed=passed and lines==expected
            if args.workload=='unix_named':passed=not code and unix_named_output(raw,False) and ('Linux LA root application passed' in raw if platform=='linux' else profile.root_success(raw,0))
            if args.workload=='unix_control':passed=not code and unix_control_output(raw,False,1) and ('Linux LA root application passed' in raw if platform=='linux' else profile.root_success(raw,0))
            if args.workload=='unix_seqpacket':passed=not code and unix_seqpacket_output(raw,False,1) and ('Linux LA root application passed' in raw if platform=='linux' else unix_seqpacket_owners(raw,'loongarch'))
            if args.workload=='sysv_msg':passed=not code and sysv_msg_output(raw,1) and ('Linux LA root application passed' in raw if platform=='linux' else unix_seqpacket_owners(raw,'loongarch'))
            if args.workload=='memory':passed=not code and memory_output(raw) and ('Linux LA root application passed' in raw if platform=='linux' else profile.root_success(raw,0))
            if args.workload=='sysv_sem':passed=not code and sysv_sem_output(raw,1) and ('Linux LA root application passed' in raw if platform=='linux' else unix_seqpacket_owners(raw,'loongarch'))
            if args.workload=='mremap':passed=not code and mremap_output(raw,1) and ('Linux LA root application passed' in raw if platform=='linux' else profile.root_success(raw,0))
            row={'passed':passed,'qemu_returncode':code,'kernel_sha256':digest(snapshot),'command':command,
                'owner_baseline_checked':platform=='boaros'}
            metadata['runs'][f'{platform}-{memory}']=row
            (work/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
            print(platform,memory,'PASS' if passed else 'FAIL',work,flush=True)
            if not passed:print('\n'.join(raw.splitlines()[-20:]));return 1
            with (case/'fsck.log').open('w') as log:subprocess.run(['e2fsck','-fn',str(image)],stdout=log,stderr=subprocess.STDOUT,check=True)
            image.unlink();snapshot.unlink()
    return 0

def run_smp(args):
    sys.path.insert(0,str(ROOT/'tests/smp'))
    from user import boot_user,identity
    output=args.output or ROOT/'build/network'/('socket-smp-'+str(time.time_ns()))
    output.mkdir(parents=True,exist_ok=False)
    compiler=ROOT/'build/riscv/musl-root/bin/musl-gcc'
    program=output/'program'
    compile_command=[str(compiler),*PROFILES['riscv'].musl_flags(compiler),
        *(['-Wl,--dynamic-linker=/lib/ld-musl-riscv64.so.1'] if args.dynamic else ['-static']),*(['-DEVENT_SMOKE=1'] if args.workload=='event' and not args.observe else []),'-O2','-pthread','-Wall','-Wextra','-Werror',
        str(workload_source(args.workload)),'-o',str(program)]
    with (output/'compile.log').open('w') as log:subprocess.run(compile_command,stdout=log,stderr=subprocess.STDOUT,check=True)
    (output/'compile-command.json').write_text(json.dumps(compile_command,indent=2)+'\n')
    variants=[]
    if args.only!='boaros':
        linux,linux_identity=harness.linux_build(ROOT/'tests/smp/linux-user.config')
        config=(linux.parents[3]/'.config').read_text()
        if 'CONFIG_NR_CPUS=4' not in config:raise RuntimeError('four-CPU reference required')
        variants.append(('linux',linux))
    if args.only!='linux':variants.append(('boaros',args.kernel or ROOT/'kernel-rv'))
    originals=variants;variants=[]
    for name,kernel in originals:
        snapshot=output/(name+'-kernel');shutil.copyfile(kernel,snapshot);variants.append((name,snapshot))
    report={'program':identity(program),'dynamic':args.dynamic,'runs':[],'status':'running'}
    (output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    expected=[f'SOCKET-SMP CASE {name} OK' for name in ('ipv4-tcp','ipv6-tcp','ipv4-udp','ipv6-udp','unix-stream','unix-dgram','blocking-close-reuse-cancel','fault-empty-trunc-timeout-sendfile','shared-readers-writers','remote-epoll-oneshot-nested')]+['SOCKET-SMP END 10']
    pipe=args.workload=='pipe';event=args.workload=='event';altstack=args.workload=='altstack'
    unix_named=args.workload=='unix_named'
    unix_control=args.workload=='unix_control'
    unix_seqpacket=args.workload=='unix_seqpacket'
    sysv_sem=args.workload=='sysv_sem'
    sysv_msg=args.workload=='sysv_msg';memory_workload=args.workload=='memory'
    mremap=args.workload=='mremap'
    futex=args.workload=='futex'
    posix=args.workload=='posix'
    queued=args.workload=='queued'
    if altstack:expected=[f'ALTSTACK CASE {name} OK' for name in ('abi','frames','bad-stacks','lifecycle','threads')]+['ALTSTACK CASE copy-races SKIP','ALTSTACK END 6']
    if event:expected=[f'EVENT CASE {name} OK' for name in ('counter-flags-limit','vectors-fault-alias','timer-clocks-periodic','timer-fault-commit','eventfd-direct','eventfd-epoll','timerfd-remote','lifecycle-close-reuse-signal-exec')]+['EVENT END 8']
    if pipe:expected=[f'PIPE CASE {name} OK' for name in ('stream-one','stream-four','ppoll-four','pselect-four','epoll-four','close-reuse-eof-signal','vectors-fault-fifo-sendfile-mixed-poll','epoll-modes-fault-rearm')]+['PIPE END 8']
    try:
        for cpus in args.cpus or [4]:
            for memory in args.memory or ['512M']:
                if posix:expected=['POSIX CASE objects OK','POSIX CASE faults-quota OK','POSIX CASE lifecycle OK','POSIX CASE wall OK','POSIX CASE shared-standard-signal OK','POSIX CASE periodic-overrun OK','POSIX CASE stale-ignore-forged OK','POSIX CASE exec-pending OK','POSIX CASE cpu-clocks OK','POSIX CASE cpu-target-exit OK','POSIX CASE handler-libc-thread OK','POSIX CASE native-output-fault OK','POSIX CASE pending-oneshot OK','POSIX CASE group-controls OK','POSIX CASE caller-cpus OK','POSIX END failures=0']
                if queued:expected=[f'QUEUED THREADS mode={mode} cpus={cpus} OK' for mode in range(4)]
                folder=output/f'{cpus}cpu-{memory}';folder.mkdir()
                fixture=harness.fixture(folder,program)
                if args.performance:
                    marker=folder/'performance';marker.write_text('fixed named AF_UNIX performance workload\n')
                    batch=folder/'performance.debugfs';batch.write_text(f'write "{marker.resolve()}" /performance\n')
                    harness.run_logged(['debugfs','-w','-f',str(batch),str(fixture)],folder/'performance-install.log')
                if args.dynamic:
                    loader=ROOT/'build/riscv/musl-root/lib/libc.so'
                    batch=folder/'loader.debugfs';batch.write_text(f'mkdir /lib\nwrite "{loader}" /lib/ld-musl-riscv64.so.1\nset_inode_field /lib/ld-musl-riscv64.so.1 mode 0100755\n')
                    harness.run_logged(['debugfs','-w','-f',str(batch),str(fixture)],folder/'loader.log')
                for target,kernel in variants:
                    disk=folder/(target+'.img');shutil.copyfile(fixture,disk)
                    if target=='boaros' and args.observe:
                        marker=folder/'socket-observer';marker.write_text('private correctness observations\n')
                        batch=folder/'observer.debugfs';batch.write_text(f'write "{marker.resolve()}" /'+('futex-observer' if futex else 'posix-observer' if posix else 'queued-observer' if queued else 'altstack-observer' if altstack else 'event-observer' if event else 'pipe-observer' if pipe else 'socket-observer')+'\n')
                        harness.run_logged(['debugfs','-w','-f',str(batch),str(disk)],folder/'observer-install.log')
                    raw,record,_=boot_user('qemu-system-riscv64',args.firmware,str(kernel.resolve()),disk,memory,cpus,folder,target=='linux')
                    lines=[line[line.index('SOCKET-SMP '):] for line in raw.replace('\r','').splitlines() if 'SOCKET-SMP ' in line]
                    if posix:lines=[line for line in raw.replace('\r','').splitlines() if line.startswith('POSIX ')]
                    if queued:lines=[line for line in raw.replace('\r','').splitlines() if line.startswith('QUEUED ')]
                    if pipe:lines=[line for line in raw.replace('\r','').splitlines() if line.startswith('PIPE CASE ') or line.startswith('PIPE END ')]
                    if event:lines=[line for line in raw.replace('\r','').splitlines() if line.startswith('EVENT CASE ') or line.startswith('EVENT END ')]
                    if altstack:
                        lines=[line for line in raw.replace('\r','').splitlines() if line.startswith('ALTSTACK CASE ') or line.startswith('ALTSTACK END ')]
                        expected[-2]='ALTSTACK CASE copy-races '+('OK' if target=='boaros' and args.observe and cpus>1 else 'SKIP')
                    if queued:expected=[f'QUEUED THREADS mode={mode} cpus={cpus} OK' for mode in range(4)]+['QUEUED COMPETE OK']+(['QUEUED CONTROL callback-source-exit OK','QUEUED CONTROL prepared-leader-exit OK','QUEUED CONTROL prepared-thread-exec OK'] if target=='boaros' and args.observe and cpus>1 else ['QUEUED CONTROL SKIP'])+['QUEUED END failures=0']
                    if futex:lines,expected=futex_lines(raw,cpus)
                    passed=bool(expected) and lines==expected and (target=='linux' or PROFILES['riscv'].root_success(raw,0))
                    if unix_named:passed=unix_named_output(raw,args.performance) and (target=='linux' or PROFILES['riscv'].root_success(raw,0))
                    if unix_control:passed=unix_control_output(raw,args.performance,cpus) and (target=='linux' or PROFILES['riscv'].root_success(raw,0))
                    if unix_seqpacket:passed=unix_seqpacket_output(raw,args.performance,cpus) and (target=='linux' or unix_seqpacket_owners(raw,'riscv'))
                    if sysv_msg:passed=sysv_msg_output(raw,cpus) and (target=='linux' or unix_seqpacket_owners(raw,'riscv'))
                    if memory_workload:
                        online=[int(v) for v in re.findall(r'^BoarOS: SMP online logical=(\d+) hart=0x[0-9a-f]+\r?$',raw,re.M)]
                        passed=memory_output(raw) and (target=='linux' or PROFILES['riscv'].root_success(raw,0) and online==list(range(1,cpus)))
                    if sysv_sem:passed=sysv_sem_output(raw,cpus) and (target=='linux' or unix_seqpacket_owners(raw,'riscv'))
                    if mremap:passed=mremap_output(raw,cpus) and (target=='linux' or PROFILES['riscv'].root_success(raw,0))
                    observations=None
                    if target=='boaros' and args.observe and not pipe and not event and not altstack and not queued and not posix and not futex and not mremap and not unix_named and not unix_control and not unix_seqpacket and not sysv_sem and not sysv_msg and not memory_workload:
                        matches=re.findall(r'SOCKET caller cpu=(0x[0-9a-f]+) operations=(0x[0-9a-f]+) copy-bytes=(0x[0-9a-f]+) pins=(0x[0-9a-f]+) waits=(0x[0-9a-f]+)',raw)
                        observations=[[int(value,16) for value in row] for row in matches]
                        active=[row for row in observations if row[0]<cpus]
                        holds=re.findall(r'SOCKET controlled rx=(0x[0-9a-f]+) tx-pin=(0x[0-9a-f]+) callback=(0x[0-9a-f]+)',raw)
                        controlled=[line for line in raw.replace('\r','').splitlines() if line.startswith('SOCKET-CONTROL ')]
                        passed=passed and len(active)==cpus and {row[0] for row in active}==set(range(cpus)) and all(all(row[1:]) for row in active)
                        passed=passed and len(holds)==1 and all(int(value,16)==1 for value in holds[0]) and controlled==[
                            'SOCKET-CONTROL reservation-append-last-close OK','SOCKET-CONTROL tx-pin-remote-mprotect OK','SOCKET-CONTROL callback-borrow-del-close OK']
                        passed=passed and bool(re.search(r'SOCKET CPU0 protocol-calls=0x[1-9a-f][0-9a-f]*',raw))
                    if target=='boaros' and args.observe and pipe:
                        observations=[[int(v,16) for v in row] for row in re.findall(r'PIPE caller cpu=(0x[0-9a-f]+) reads=(0x[0-9a-f]+) writes=(0x[0-9a-f]+) queries=(0x[0-9a-f]+) epoll-queries=(0x[0-9a-f]+) event-bytes=(0x[0-9a-f]+)',raw)]
                        active=[row for row in observations if row[0]<cpus]
                        passed=passed and len(active)==cpus and {row[0] for row in active}==set(range(cpus)) and all(all(row[1:]) for row in active)
                    if target=='boaros' and args.observe and event:
                        observations=[[int(v,16) for v in row] for row in re.findall(r'EVENT caller cpu=(0x[0-9a-f]+) io=(0x[0-9a-f]+) completed-bytes=(0x[0-9a-f]+) pins=(0x[0-9a-f]+) timers=(0x[0-9a-f]+)',raw)]
                        active=[row for row in observations if row[0]<cpus]
                        passed=passed and len(active)==cpus and {row[0] for row in active}==set(range(cpus)) and all(all(row[1:]) for row in active) and bool(re.search(r'EVENT CPU0 expiry=0x[1-9a-f][0-9a-f]*',raw))
                    if target=='boaros' and args.observe and altstack:
                        observations=[[int(v,16) for v in row] for row in re.findall(r'ALTSTACK caller cpu=(0x[0-9a-f]+) frames=(0x[0-9a-f]+) returns=(0x[0-9a-f]+)',raw)]
                        active=[row for row in observations if row[0]<cpus]
                        passed=passed and len(active)==cpus and {row[0] for row in active}==set(range(cpus)) and all(all(row[1:]) for row in active) and all(not any(row[1:]) for row in observations if row[0]>=cpus)
                    if target=='boaros' and args.observe and posix:
                        observations=[[int(v,16) for v in row] for row in re.findall(r'POSIX caller cpu=(0x[0-9a-f]+) settings=(0x[0-9a-f]+) consumers=(0x[0-9a-f]+)',raw)]
                        active=[row for row in observations if row[0]<cpus]
                        passed=passed and len(active)==cpus and all(all(row[1:]) for row in active) and 'POSIX expiry CPU0 OK' in raw
                    if target=='boaros' and args.observe and futex:
                        observations=[[int(v,16) for v in row] for row in re.findall(r'FUTEX caller cpu=(0x[0-9a-f]+) reads=(0x[0-9a-f]+) updates=(0x[0-9a-f]+) keys=(0x[0-9a-f]+)',raw)]
                        active=[row for row in observations if row[0]<cpus]
                        passed=passed and len(active)==cpus and all(all(row[1:]) for row in active)
                    if target=='boaros' and args.observe and queued:
                        observations=[[int(v,16) for v in row] for row in re.findall(r'QUEUED caller cpu=(0x[0-9a-f]+) waits=(0x[0-9a-f]+) reads=(0x[0-9a-f]+) queries=(0x[0-9a-f]+) sends=(0x[0-9a-f]+)',raw)]
                        active=[row for row in observations if row[0]<cpus]
                        passed=passed and 'QUEUED send-mismatch=0x0' in raw and len(active)==cpus and all(all(row[1:]) for row in active)
                        passed=passed and 'QUEUED source-holds=0x1 prepared-holds=0x2' in raw and 'QUEUED CONTROL prepared-thread-exec OK' in raw
                    row=dict(target=target,cpus=cpus,memory=memory,passed=passed,observations=observations,**record);report['runs'].append(row)
                    print(target,cpus,memory,'PASS' if passed else 'FAIL',output,flush=True)
                    if not passed:raise RuntimeError('socket SMP stream or owner cleanup failed: '+str(folder))
                    with (folder/(target+'-fsck.log')).open('w') as log:subprocess.run(['e2fsck','-fn',str(disk)],stdout=log,stderr=subprocess.STDOUT,check=True)
                    disk.unlink()
                fixture.unlink()
        report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=str(error));raise
    finally:(output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    return 0

def main(default_arch="riscv"):
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--only',choices=('linux','boaros'))
    parser.add_argument('--workload',choices=('contract','content','timer','budget','interface','sendfile','admission','unix_sender','unix_named','unix_control','unix_seqpacket','smp','epoll','pipe','event','altstack','queued','posix','futex','mremap','sysv_sem','sysv_msg','memory'),default='contract')
    parser.add_argument('--platform-config',choices=('fixture','official'),default='fixture')
    parser.add_argument('--arch',choices=('riscv','loongarch'),default=default_arch)
    parser.add_argument('--memory',action='append',choices=('512M','1G'))
    parser.add_argument('--kernel',type=Path)
    parser.add_argument('--cpus',action='append',type=int,choices=(1,2,4))
    parser.add_argument('--performance',action='store_true',help='run fixed AF_UNIX traffic via /performance')
    parser.add_argument('--dynamic',action='store_true')
    parser.add_argument('--observe',action='store_true')
    parser.add_argument('--output',type=Path)
    parser.add_argument('--firmware',default='default')
    args=parser.parse_args()
    if args.performance and args.arch!='riscv':parser.error('--performance requires the RISC-V SMP runner')
    if args.performance and args.workload not in ('unix_named','unix_control','unix_seqpacket'):parser.error('--performance requires --workload unix_named, unix_control or unix_seqpacket')
    if args.workload in ('unix_named','unix_control','unix_seqpacket','smp','pipe','event','altstack','queued','posix','futex','mremap','sysv_sem','sysv_msg','memory') and args.arch=='riscv':return run_smp(args)
    if args.arch=='riscv' and args.platform_config=='official' and args.memory and args.memory!=['1G']:
        parser.error('official profile requires 1G')
    if args.arch=='loongarch':return run_loongarch(args)
    args.kernel=args.kernel or ROOT/'kernel-rv'
    work=ROOT/'build/network'/('contract-'+str(time.time_ns()))
    work.mkdir(parents=True)
    program=work/'init'
    compiler=ROOT/'build/riscv/musl-root/bin/musl-gcc'
    subprocess.run([str(compiler),*PROFILES['riscv'].musl_flags(compiler),'-static','-O2','-pthread','-Wall','-Wextra','-Werror',
                    str(workload_source(args.workload)),'-o',str(program)],check=True)
    image=harness.fixture(work,program)
    metadata={'program_sha256':digest(program),'fixture_sha256':digest(image),'runs':{}}
    variants=[]
    if args.only!='boaros':
        linux,identity=harness.fixed_linux_image(None)
        metadata['linux']=identity;variants.append(('linux',linux))
    if args.only!='linux':variants.append(('boaros',args.kernel))
    qemu=os.environ.get('QEMU_RISCV64','qemu-system-riscv64')
    metadata['qemu']=subprocess.check_output([qemu,'--version'],text=True)
    for name,kernel in variants:
        snapshot=work/(name+'-kernel');shutil.copyfile(kernel,snapshot)
        memories=args.memory or (('1G',) if args.platform_config=='official' else ('512M',))
        for memory in memories:
            label=name if len(memories)==1 else name+'-'+memory
            disk=work/(label+'.img');shutil.copyfile(image,disk)
            command=[qemu,'-machine','virt','-bios','default','-kernel',str(snapshot),'-m',memory,'-smp','1',
                     '-nographic','-no-reboot','-drive',f'file={disk},if=none,format=raw,id=root',
                     '-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
            if args.platform_config=='official':
                command+=['-device','virtio-net-device,netdev=net','-netdev','user,id=net','-rtc','base=utc']
            if name=='linux':command+=['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
            log=work/(label+'.log')
            try:harness.run_logged(command,log,60)
            except (RuntimeError,TimeoutError):pass
            raw=log.read_text(errors='replace')
            passed=('POSIX END failures=0' if args.workload=='posix' else 'QUEUED END failures=0' if args.workload=='queued' else 'ALTSTACK END 6' if args.workload=='altstack' else 'EPOLL PASS delivery' if args.workload=='epoll' else 'PIPE END 8' if args.workload=='pipe' else f'NETWORK PASS {args.workload}') in raw
            if name=='boaros':passed=passed and 'heap-live=0x0; shutting down' in raw and 'exited status=0x0 ' in raw
            metadata['runs'][label]={'passed':passed,'memory':memory,'kernel_sha256':digest(snapshot),'command':command}
            print(name,memory,'PASS' if passed else 'FAIL',work)
            if not passed:print('\n'.join(raw.splitlines()[-8:]))
    (work/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
    if not all(row['passed'] for row in metadata['runs'].values()):return 1
    for path in work.glob('*.img'):path.unlink()
    return 0

if __name__=='__main__':sys.exit(main())
