"""Real RNG IRQ, entropy gates and lifecycle; controlled EGD is not an entropy-quality test."""
import argparse
import hashlib
import json
from pathlib import Path
import selectors
import shutil
import socket
import subprocess
import struct
import sys
import tempfile
import time
from arch_profiles import PROFILES,ROOT

def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def quiet(command):subprocess.run(list(map(str,command)),check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
def unseeded_boot(command,work):
    """Control the firmware input; fixed LA Linux consumes FDT seed before early params."""
    dtb=work/'firmware.dtb'
    dumped=list(command);index=dumped.index('-machine')+1
    dumped[index]+=f',dumpdtb={dtb}'
    quiet(dumped)
    data=dtb.read_bytes()
    if len(data)<40 or struct.unpack_from('>I',data)[0]!=0xd00dfeed:raise RuntimeError('invalid firmware FDT')
    size,position,strings=struct.unpack_from('>III',data,4)
    if size>len(data) or position>=size or strings>=size:raise RuntimeError('invalid firmware FDT bounds')
    stack=[];found=None
    while position<size:
        token_at=position;token=struct.unpack_from('>I',data,position)[0];position+=4
        if token==1:
            end=data.index(0,position,size);stack.append(data[position:end].decode());position=(end+4)&~3
        elif token==2:stack.pop()
        elif token==3:
            length,name_offset=struct.unpack_from('>II',data,position);position+=8
            name_at=strings+name_offset
            name=data[name_at:data.index(0,name_at,size)].decode()
            end=(position+length+3)&~3
            if end>size:raise RuntimeError('invalid firmware property')
            if stack==['','chosen'] and name=='rng-seed':found=(token_at,end-token_at)
            position=end
        elif token==4:pass
        elif token==9:break
        else:raise RuntimeError('unknown firmware FDT token')
    if not found:raise RuntimeError('firmware RNG seed missing from control input')
    offset,length=found
    return {'physical':0x100000+offset,'length':length,'operation':'FDT_NOP boot seed'}
def control_seed(profile,port,control,work):
    compiler=Path(shutil.which(profile.compiler))
    gdb=compiler.with_name(compiler.name[:-3]+'gdb')
    lines=['set pagination off',f'target remote 127.0.0.1:{port}',
        f'if *(unsigned int*)0x{control["physical"]:x} != 0x03000000', 'quit 1','end']
    for at in range(control['physical'],control['physical']+control['length'],4):
        lines += [f'set {{unsigned int}}0x{at:x}=0x04000000',
            f'if *(unsigned int*)0x{at:x} != 0x04000000','quit 1','end']
    lines+=['detach']
    script=work/'seed.gdb';script.write_text('\n'.join(lines)+'\n')
    result=subprocess.run([str(gdb),'-nx','-batch','-x',str(script)],
        stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=20)
    (work/'seed-control.log').write_bytes(result.stdout)
    if result.returncode:raise RuntimeError('paused firmware seed control failed')
    control['tool_sha256']=digest(gdb)
def supervisor(profile,work):
    sys.path.insert(0,str(ROOT/'tests/loongarch'))
    from reference import archive
    program=work/'linux-init'
    quiet([profile.compiler,*profile.raw_flags,'-O2','-DEXPECTED_EXIT_STATUS=42','-DROOT_MOUNT_FLAGS=1','-ffreestanding',
        '-fno-builtin','-fno-stack-protector','-nostdlib','-nostartfiles','-static','-no-pie',
        '-Wl,--build-id=none',f'-Wl,-z,max-page-size={profile.page_size}',
        '-T','tests/common/user.ld','tests/loongarch/root_linux_init.c','tests/common/user_start.S','-o',program])
    initrd=work/'initramfs.gz'
    initrd.write_bytes(archive([('dev',0o040755,b'',0,0),('dev/console',0o020600,b'',5,1),
        ('init',0o100755,program.read_bytes(),0,0),('TRAILER!!!',0,b'',0,0)]))
    return initrd
def run(args,profile,platform,memory,transport,mode):
    work=args.output/f'{platform}-{memory}-{transport}-{mode}';work.mkdir()
    guest=connection=server=None;good=False
    try:
        kernel=Path(args.linux_kernel or profile.linux_kernel()) if platform=='Linux' else Path(args.kernel)
        shutil.copyfile(kernel,work/'kernel')
        disk=work/'root.img'
        with disk.open('wb') as stream:stream.truncate(32*1024*1024)
        quiet(['mkfs.ext4','-q','-F','-b','4096','-O','^metadata_csum,^64bit,^orphan_file',disk])
        (work/'mode').write_text(mode)
        for command in (f'write {args.program} /init','set_inode_field /init mode 0100755',f'write {work}/mode /mode'):
            quiet(['debugfs','-w','-R',command,disk])
        command=profile.boot(args.qemu,work/'kernel',memory)
        command+=['-drive',f'file={disk},if=none,format=raw,id=root,readonly=on','-device',profile.block(transport),'-net','none']
        if profile.name=='riscv':command+=['-global','virtio-mmio.force-legacy='+('true' if transport=='legacy' else 'false')]
        if platform=='Linux':command+=['-initrd',str(supervisor(profile,work)),'-append',
            'console=ttyS0 rdinit=/init loglevel=3 random.trust_cpu=off random.trust_bootloader=off']
        seed_control=None
        port=None
        if profile.name=='loongarch':
            seed_control=unseeded_boot(command,work)
            with socket.socket() as reserved:
                reserved.bind(('127.0.0.1',0));port=reserved.getsockname()[1]
            command+=['-S','-gdb',f'tcp:127.0.0.1:{port}']
        if mode=='n' or (mode=='a' and args.force_device):command+=['-object','rng-random,id=entropy,filename=/dev/urandom']
        elif mode in ('d','s'):
            server=socket.socket(socket.AF_UNIX);server.bind(str(work/'egd.sock'));server.listen(1)
            command+=['-chardev',f'socket,id=egd,path={work}/egd.sock','-object','rng-egd,id=entropy,chardev=egd']
        if mode!='a' or args.force_device:command+=['-device',profile.rng()]
        identity={'arch':profile.name,'platform':platform,'ram':memory,'transport':transport,'mode':mode,
            'program':digest(args.program),'kernel':digest(kernel),'qemu':digest(shutil.which(args.qemu) or args.qemu),
            'command':command,'seed_control':seed_control}
        (work/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
        guest=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,bufsize=0)
        if seed_control:
            control_seed(profile,port,seed_control,work)
            (work/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
        select=selectors.DefaultSelector();select.register(guest.stdout,selectors.EVENT_READ,'guest')
        if server:
            server.settimeout(10);connection,_=server.accept();connection.setblocking(False)
            select.register(connection,selectors.EVENT_READ,'egd')
        log=bytearray();requests=bytearray();released=False;total=0;end=time.monotonic()+args.timeout
        while select.get_map() and time.monotonic()<end:
            for key,_ in select.select(.1):
                data=key.fileobj.recv(4096) if key.data=='egd' else key.fileobj.read(4096)
                if not data:select.unregister(key.fileobj);continue
                if key.data=='guest':log.extend(data);(work/'guest.log').write_bytes(log)
                else:
                    requests.extend(data)
                    while len(requests)>=2:
                        assert requests[0]==2 and 0<requests[1]<=64,bytes(requests)
                        total+=requests[1];del requests[:2]
                if mode=='d' and total and b'rng: computation progressed' in log and not released:
                    assert b'rng: unready' in log and b'rng: waiter ready' not in log
                    connection.sendall(bytes(i%251 for i in range(total)));released=True
            if guest.poll() is not None:break
        guest.wait(timeout=2)
        # process exit does not consume bytes already buffered in the stdout pipe.
        log.extend(guest.stdout.read());(work/'guest.log').write_bytes(log)
        output=log.decode(errors='replace')
        assert not guest.returncode and 'fatal' not in output,output
        assert all(marker in output for marker in args.marker),output
        if platform=='Linux':assert ('Linux '+('LA' if profile.name=='loongarch' else 'RV')+' root application passed') in output,output
        else:assert profile.root_success(output,42),output
        if mode=='d':assert released and 'rng: delayed ready' in output,output
        elif mode=='s':assert total and 'rng: stop pending' in output,output
        elif mode=='a':assert 'rng: absent boot ok' in output and 'rng: unready wait interrupted' in output,output
        else:assert 'rng: ready' in output,output
        if platform=='BoarOS' and profile.name=='riscv':quiet(['python3','-B',ROOT/'tests/check-stack-report.py',work/'guest.log'])
        if digest(args.program)!=identity['program'] or digest(kernel)!=identity['kernel']:raise RuntimeError('RNG input changed during run')
        (work/'result.json').write_text(json.dumps({'qemu_exit':guest.returncode,'user_exit':42,
            'completed':True,'pass':True,'released_entropy':released,'egd_requested':total,
            'root_owner_checked':platform=='BoarOS'},indent=2)+'\n')
        scope='ELF/exit and root mount lifecycle' if platform=='Linux' else 'ELF/exit and page/heap/stack/device owners'
        good=True;print(f'RNG {profile.name}/{platform}/{memory}/{transport} mode={mode}: {scope} PASS',flush=True)
    finally:
        if guest and guest.poll() is None:guest.kill();guest.wait()
        if connection:connection.close()
        if server:server.close()
        if good:
            for path in work.iterdir():
                if path.suffix not in ('.log','.json'):path.unlink()
        else:print(f'RNG failure artifacts: {work}',flush=True)
def main(default_arch='riscv'):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch',choices=PROFILES,default=default_arch)
    parser.add_argument('--kernel');parser.add_argument('--linux-kernel');parser.add_argument('--program');parser.add_argument('--qemu')
    parser.add_argument('--transport',choices=('all','legacy','modern'),default='all')
    parser.add_argument('--platform',choices=('Linux','BoarOS'),action='append');parser.add_argument('--mode',choices=('n','a','d','s'),action='append')
    parser.add_argument('--timeout',type=float,default=60)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--marker',action='append',default=[])
    parser.add_argument('--force-device',action='store_true',help='keep hardware present for injected construction-failure probes')
    args=parser.parse_args();profile=PROFILES[args.arch]
    args.kernel=args.kernel or profile.kernel;args.qemu=args.qemu or profile.qemu
    args.program=args.program or ('build/loongarch/rng-probe' if args.arch=='loongarch' else 'build/riscv/tests/user/rng-rv')
    if args.output:args.output.mkdir(parents=True,exist_ok=False)
    else:args.output=Path(tempfile.mkdtemp(prefix='rng-run.',dir=ROOT/'build'/('loongarch' if args.arch=='loongarch' else 'riscv')))
    transports=['modern'] if args.arch=='loongarch' else ['legacy','modern'] if args.transport=='all' else [args.transport]
    platforms=args.platform or (['Linux','BoarOS'] if args.arch=='loongarch' else ['BoarOS'])
    for platform in platforms:
        for memory in (('512M','1G') if args.arch=='loongarch' else ('512M',)):
            for transport in transports:
                for mode in args.mode or ('n','a','d','s'):run(args,profile,platform,memory,transport,mode)
    print(f'RNG results and input identities: {args.output}',flush=True)
if __name__=='__main__':main()
