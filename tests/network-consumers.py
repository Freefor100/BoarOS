#!/usr/bin/env python3
"""Run untouched network consumer ELF/script inputs in a real loopback guest."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests/diff-abi'))
import harness

def digest(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def consumer_results(raw,suite):
    results=[]
    pattern=r'^NETWORK COMMAND BEGIN name=(\S+) argv=(.*?)\n(.*?)^NETWORK COMMAND END name=\1 wait=(\d+) timeout=(\d+) elapsed_ms=(\d+)$'
    for match in re.finditer(pattern,raw,re.M|re.S):
        name,argv,output,status,timeout,elapsed=match.groups()
        row={'name':name,'argv':argv.split(),'wait_status':int(status),'timeout':bool(int(timeout)),
             'elapsed_ms':int(elapsed),'output':output,'metrics':[]}
        context=re.findall(r'^NETWORK INPUT libc=(\S+) suite=(\S+)',raw[:match.start()],re.M)
        row['libc'],row['suite']=context[-1]
        valid=int(status)==0 and int(timeout)==0
        if row['suite']=='iperf':
            receivers=re.findall(r'^\[\s*(\d+)\]\s+([\d.]+)-([\d.]+)\s+sec\s+([\d.]+)\s+(\w*Bytes)\s+([\d.]+)\s+(\w*bits/sec).*?receiver\s*$',output,re.M)
            for connection,start,end,amount,unit,rate,rate_unit in receivers:
                scale={'Bytes':1,'KBytes':1024,'MBytes':1024**2,'GBytes':1024**3}.get(unit)
                if scale is None:raise ValueError('unknown original iperf transfer unit '+unit)
                row['metrics'].append({'connection':int(connection),'elapsed_s':float(end)-float(start),
                    'received_bytes_rounded':float(amount)*scale,'throughput':float(rate),'unit':rate_unit})
            expected=5 if name.startswith('PARALLEL') else 1
            valid=valid and len({m['connection'] for m in row['metrics']})==expected and all(
                m['elapsed_s']>=1.5 and m['received_bytes_rounded']>0 and m['throughput']>0 for m in row['metrics'])
            row['tcp_info_verified']=False
        else:
            numbers=[]
            for line in output.splitlines():
                values=line.split()
                if values and all(re.fullmatch(r'-?\d+(?:\.\d+)?',v) for v in values):numbers.append([float(v) for v in values])
            if name=='UDP_STREAM':
                sender=next((v for v in numbers if len(v)==6),None)
                receiver=next((v for v in numbers if len(v)==4),None)
                if sender and receiver:
                    row['metrics']=[{'elapsed_s':receiver[1],'received_messages':int(receiver[2]),
                        'received_bytes':int(receiver[2]*sender[1]),'sent_messages':int(sender[3]),
                        'send_errors':int(sender[4]),'throughput':receiver[3],'unit':'Mbits/sec'}]
            elif name=='TCP_STREAM':
                values=next((v for v in numbers if len(v)==5),None)
                if values:row['metrics']=[{'elapsed_s':values[3],'throughput':values[4],'unit':'Mbits/sec'}]
            else:
                values=next((v for v in numbers if len(v)==6),None)
                if values:row['metrics']=[{'elapsed_s':values[4],'throughput':values[5],'unit':'transactions/sec'}]
            valid=valid and len(row['metrics'])==1 and row['metrics'][0]['elapsed_s']>=0.8 and row['metrics'][0]['throughput']>0
            if name=='UDP_STREAM':valid=valid and row['metrics'][0].get('received_messages',0)>0
        row['valid']=bool(valid);results.append(row)
    return results

def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--only',choices=('linux','boaros'))
    parser.add_argument('--libc',choices=('musl','glibc','both'),default='musl')
    parser.add_argument('--suite',choices=('iperf','netperf','both'),default='iperf')
    parser.add_argument('--case',choices=('tcp','udp5','all','representative','script'),default='tcp')
    parser.add_argument('--trace',action='store_true')
    parser.add_argument('--observe',action='store_true',help='one combined representative boot on a COST kernel')
    parser.add_argument('--repeat',type=int,default=1)
    parser.add_argument('--kernel',type=Path,default=ROOT/'kernel-rv')
    args=parser.parse_args()
    if args.observe and (args.only!='boaros' or args.libc!='both' or args.suite!='both' or args.case!='representative' or args.repeat!=1):
        parser.error('observation requires one BoarOS boot with both libcs/suites and representative cases')
    work=ROOT/'build/network'/('consumers-'+str(time.time_ns()));work.mkdir(parents=True)
    program=work/'init'
    subprocess.run([str(ROOT/'build/riscv/musl-root/bin/musl-gcc'),'-fno-link-libatomic','-static','-O2',
        '-Wall','-Wextra','-Werror',str(ROOT/'tests/workloads/network/consumers.c'),'-o',str(program)],check=True)
    original=ROOT/'references/oscomp-autotest/sdcard-rv.img'
    if digest(original)!='f419468678d342133546add2f8459ea09aeba987ba968e28753d6ee656996b8b':raise RuntimeError('fixed input image changed')
    fixture=harness.fixture(work,program);commands=['mkdir /proc','mkdir /tmp','mkdir /lib','cd /dev','mknod urandom c 1 9','set_inode_field urandom mode 020666','mknod random c 1 8','set_inode_field random mode 020666','cd /']
    identities={};libcs=('musl','glibc') if args.libc=='both' else (args.libc,)
    suites=('iperf','netperf') if args.suite=='both' else (args.suite,)
    for libc in libcs:
        commands.extend(['mkdir /'+libc,'mkdir /'+libc+'/lib'])
        for name in ('iperf3','netperf','netserver','busybox','iperf_testcode.sh','netperf_testcode.sh'):
            target=work/(libc+'-'+name)
            subprocess.run(['debugfs','-R',f'dump /{libc}/{name} {target}',str(original)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            identities[f'/{libc}/{name}']=digest(target)
            commands.extend([f'write {target} /{libc}/{name}',f'set_inode_field /{libc}/{name} mode 0100755'])
        libraries=('libc.so',) if libc=='musl' else ('libc.so.6','libm.so.6','ld-linux-riscv64-lp64d.so.1')
        for name in libraries:
            target=work/(libc+'-lib-'+name)
            subprocess.run(['debugfs','-R',f'dump /{libc}/lib/{name} {target}',str(original)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            identities[f'/{libc}/lib/{name}']=digest(target)
            commands.extend([f'write {target} /{libc}/lib/{name}',f'set_inode_field /{libc}/lib/{name} mode 0100755'])
        loader='ld-musl-riscv64-sf.so.1' if libc=='musl' else 'ld-linux-riscv64-lp64d.so.1'
        source=f'/{libc}/lib/'+('libc.so' if libc=='musl' else loader)
        commands.append(f'symlink /lib/{loader} {source}')
    command_file=work/'inputs.debugfs';command_file.write_text('\n'.join(commands)+'\n')
    subprocess.run(['debugfs','-w','-f',str(command_file),str(fixture)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    variants=[];metadata={'inputs':identities,'program_sha256':digest(program),'runs':[],
        'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'source_tree':subprocess.check_output(['git','rev-parse','HEAD^{tree}'],cwd=ROOT,text=True).strip(),
        'configuration':{'cost_diagnostics':int(args.observe),'memory_mib':512,'harts':1,
            'transport':'legacy','cache':'writeback','loopback':True},
        'compiler':subprocess.check_output([str(ROOT/'build/riscv/musl-root/bin/musl-gcc'),'--version'],text=True)}
    changes=subprocess.check_output(['git','diff','HEAD'],cwd=ROOT)
    metadata['working_diff_sha256']=hashlib.sha256(changes).hexdigest()
    if args.only!='boaros':
        linux,identity=harness.fixed_linux_image(None);metadata['linux']=identity;variants.append(('linux',linux))
    if args.only!='linux':variants.append(('boaros',args.kernel))
    plugin=None
    if args.trace:
        plugin=work/'syscall-entry.so'
        flags=subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
        subprocess.run(['cc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',
            str(ROOT/'tests/workloads/diagnostics/syscall-entry-plugin.c'),'-o',str(plugin),*flags],check=True)
        metadata['plugin_sha256']=digest(plugin)
    qemu=os.environ.get('QEMU_RISCV64','qemu-system-riscv64');metadata['qemu']=subprocess.check_output([qemu,'--version'],text=True)
    metadata['qemu_sha256']=digest(shutil.which(qemu) or qemu)
    failed=False
    combined=args.libc=='both' and args.suite=='both' and args.case=='representative'
    selections=[('both','both')] if combined else list(itertools.product(libcs,suites))
    for name,kernel in variants:
        snapshot=work/(name+'-kernel');shutil.copyfile(kernel,snapshot)
        for repeat,(libc,suite) in itertools.product(range(args.repeat),selections):
            label=f'{name}-{libc}-{suite}-{repeat}';disk=work/(label+'.img');shutil.copyfile(fixture,disk)
            selection=work/'selection';selection.write_text(f'{libc} {suite} {args.case} {name}\n')
            commands=work/'selection.debugfs';commands.write_text(f'write {selection} /network-selection\n')
            subprocess.run(['debugfs','-w','-f',str(commands),str(disk)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            input_fixture_hash=digest(disk)
            command=[qemu,'-machine','virt','-bios','default','-kernel',str(snapshot),'-m','512M','-smp','1','-nographic','-no-reboot',
                '-global','virtio-mmio.force-legacy=true',
                '-object','rng-random,id=entropy,filename=/dev/urandom','-device','virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.7',
                '-drive',f'file={disk},if=none,format=raw,cache=writeback,id=root','-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
            if plugin is not None:command+=['-plugin',f'{plugin},trace={work/(label+".ecall")}']
            if name=='linux':command+=['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
            log=work/(label+'.log');print('running',label,work,flush=True)
            try:harness.run_logged(command,log,200 if args.observe else 150)
            except (RuntimeError,TimeoutError):pass
            raw=log.read_text(errors='replace').replace('\r','');passed='NETWORK CONSUMERS PASS' in raw
            records=[line for line in raw.splitlines() if line.startswith('NETWORK ')]
            if args.case=='script':
                count=6 if suite=='iperf' else 5
                subitems=re.findall(r'====== (?:iperf|netperf) (\S+) end: (success|fail) ======',raw)
                passed=passed and len(subitems)==count and all(status=='success' for _,status in subitems)
            else:
                subitems=consumer_results(raw,suite)
                expected=6 if combined else (6 if suite=='iperf' else 5) if args.case=='all' else (2 if suite=='iperf' and args.case=='representative' else 1)
                passed=passed and len(subitems)==expected and all(row['valid'] for row in subitems)
            snapshots=[]
            if args.observe:
                sys.path.insert(0,str(ROOT/'tests'));import cost_report
                for window,epoch,body in re.findall(r'^COST SNAPSHOT (\S+) (\d+)\n(.*?)^COST END$',raw,re.M|re.S):
                    snapshots.append({'name':window,'values':cost_report.parse(body,int(epoch))})
                passed=passed and len(snapshots)==6 and 'NETWORK PROTOCOL DRAIN' in raw
            elif 'COST SNAPSHOT' in raw:passed=False
            if name=='boaros':passed=passed and 'exited status=0x0 ' in raw and 'heap-live=0x0; shutting down' in raw
            protocol=[]
            for window,phase,body in re.findall(r'^NETWORK PROTOCOL name=(\S+) phase=(\S+)\n(.*?)^NETWORK PROTOCOL END$',raw,re.M|re.S):
                protocol.append({'name':window,'phase':phase,'values':dict((k,int(v)) for k,v in re.findall(r'^(\w+)=(\d+)$',body,re.M))})
            frequency=re.search(r'Platform Timer Device\s+:.*?@ (\d+)Hz',raw)
            metadata['runs'].append({'label':label,'passed':passed,'kernel_sha256':digest(snapshot),'fixture_sha256':input_fixture_hash,'command':command,'records':records,'subitems':subitems,'snapshots':snapshots,'protocol':protocol,
                'timebase_hz':int(frequency[1]) if frequency else None,
                **({'original_script_output':raw[raw.index('NETWORK INPUT'):]} if args.case=='script' and 'NETWORK INPUT' in raw else {})})
            (work/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
            print(label,'PASS' if passed else 'FAIL', '\n'+'\n'.join(records),flush=True)
            if not passed:
                print('\n'.join(raw.splitlines()[-24:]),flush=True);failed=True
                if args.case!='script':return 1
            if passed:disk.unlink()
    if not failed:fixture.unlink()
    return 1 if failed else 0
if __name__=='__main__':sys.exit(main())
