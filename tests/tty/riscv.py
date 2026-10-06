#!/usr/bin/env python3
"""Original serial shell and shared termios probes, using a raw host PTY."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import pty
import re
import selectors
import shutil
import subprocess
import sys
import tempfile
import termios
import time
import tty

ROOT = Path(__file__).resolve().parents[2]
PROMPT = rb'BOAR_TTY\$ '
sys.path.insert(0,str(ROOT/'tests'))
from arch_profiles import PROFILES


def command(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class Serial:
    def __init__(self, invocation, directory):
        self.master, slave = pty.openpty()
        tty.setraw(slave, termios.TCSANOW)
        # QEMU stdio重新开启宿主OPOST；清掉残留ONLCR等位，字节通道才不重复转换CRLF。
        settings = termios.tcgetattr(slave)
        settings[1] = 0
        termios.tcsetattr(slave, termios.TCSANOW, settings)
        self.process = subprocess.Popen(invocation, stdin=slave, stdout=slave,
                                        stderr=slave, close_fds=True)
        os.close(slave)
        os.set_blocking(self.master, False)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.master, selectors.EVENT_READ)
        self.data = bytearray()
        self.cursor = 0
        self.directory = directory
        self.inputs = []

    def send(self, data):
        self.inputs.append(data.hex())
        offset = 0
        while offset < len(data):
            try:
                offset += os.write(self.master, data[offset:])
            except BlockingIOError:
                self.pump(0.05)

    def pump(self, timeout):
        for _key, _events in self.selector.select(timeout):
            try:
                data = os.read(self.master, 65536)
            except OSError as error:
                if error.errno == errno.EIO:
                    return False
                raise
            if data:
                self.data.extend(data)
                if len(self.data) > 4 * 1024 * 1024:
                    raise AssertionError('serial output budget exceeded')
        return True

    def expect(self, pattern, timeout=20):
        deadline = time.monotonic() + timeout
        while True:
            match = re.search(pattern, self.data[self.cursor:])
            if match:
                begin = self.cursor
                self.cursor += match.end()
                return bytes(self.data[begin:self.cursor])
            if self.process.poll() is not None:
                self.pump(0)
                raise AssertionError(f'QEMU ended before {pattern!r}: {bytes(self.data[-2000:])!r}')
            if time.monotonic() >= deadline:
                raise TimeoutError(f'waiting for {pattern!r}: {bytes(self.data[-2000:])!r}')
            self.pump(min(0.2, max(0, deadline - time.monotonic())))

    def shell(self, line, marker=None):
        self.send(line.encode() + b'\n')
        if marker:
            self.expect(rb'(?m)^' + re.escape(marker.encode()) + rb'\r?\n')
        return self.expect(PROMPT)

    def finish(self):
        deadline = time.monotonic() + 30
        while self.process.poll() is None and time.monotonic() < deadline:
            self.pump(0.2)
        if self.process.poll() is None:
            raise TimeoutError('TTY exit did not complete kernel cleanup')
        self.pump(0)
        if self.process.returncode:
            raise AssertionError(f'QEMU exit {self.process.returncode}')

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.selector.close()
        os.close(self.master)
        (self.directory / 'serial.bin').write_bytes(self.data)
        (self.directory / 'inputs.txt').write_text('\n'.join(self.inputs) + '\n')


def applications(serial):
    serial.expect(PROMPT)
    serial.shell("stty rows 24 cols 80 && stty -a && printf 'TTY_STTY_A_OK\\n'", 'TTY_STTY_A_OK')
    serial.shell("saved=$(stty -g) && stty raw && stty cooked && stty sane && stty \"$saved\" && test \"$(stty -g)\" = \"$saved\" && printf 'TTY_MODES_OK\\n'", 'TTY_MODES_OK')
    serial.shell("stty rows 24 cols 80; test \"$(stty size)\" = '24 80' && printf 'TTY_SIZE_OK\\n'", 'TTY_SIZE_OK')
    serial.send(b"printf 'TTY_EDIX\x7fT_OK\\n'\n")
    serial.expect(rb'(?m)^TTY_EDIT_OK\r?\n'); serial.expect(PROMPT)
    serial.shell("test \"$(printf abcd | wc -c)\" = 4 && printf 'tty-file' >/tmp/tty-file && test \"$(cat /tmp/tty-file)\" = tty-file && printf 'TTY_FILE_OK\\n'", 'TTY_FILE_OK')
    serial.send(b'cat\n'); serial.expect(rb'(?m)^cat\r?\n')
    serial.send(b'TTY_CAT_FOREGROUND\n')
    # canonical echo与cat真正回送分别出现，第二条记录确认子进程已经运行。
    serial.expect(rb'(?m)^TTY_CAT_FOREGROUND\r?\n')
    serial.expect(rb'(?m)^TTY_CAT_FOREGROUND\r?\n')
    serial.send(b'\x03'); serial.expect(PROMPT)
    serial.shell("printf 'TTY_INT_CAT_OK\\n'", 'TTY_INT_CAT_OK')
    serial.send(b'sleep 600 & sleep_pid=$!; /gate wait-foreground "$sleep_pid" & watcher_pid=$!; fg %sleep\n')
    serial.expect(rb'(?m)^TTY_GATE_SLEEP_READY\r?\n')
    serial.send(b'\x03'); serial.expect(PROMPT)
    serial.shell('while kill -0 "$watcher_pid" 2>/dev/null; do :; done; printf "TTY_INT_SLEEP_OK\\n"', 'TTY_INT_SLEEP_OK')
    serial.send(b'/gate cpu\n'); serial.expect(rb'(?m)^TTY_GATE_CPU\r?\n')
    serial.send(b'\x03'); serial.expect(PROMPT)
    serial.shell("printf 'TTY_INT_CPU_OK\\n'", 'TTY_INT_CPU_OK')
    serial.send(b'/gate sleep\n'); serial.expect(rb'(?m)^TTY_GATE_SLEEP\r?\n')
    serial.send(b'\x1a'); serial.expect(PROMPT)
    result = serial.shell('jobs')
    if b'Stopped' not in result: raise AssertionError('Ctrl-Z job did not stop')
    serial.shell('bg')
    serial.send(b'fg\n'); serial.expect(rb'(?m)^TTY_GATE_FOREGROUND_RESUMED\r?\n')
    serial.send(b'\x03'); serial.expect(PROMPT)
    serial.shell("printf 'TTY_JOBS_OK\\n'", 'TTY_JOBS_OK')
    serial.shell('cat & cat_pid=$!')
    # The shell reports a stopped child through wait; synchronize through jobs.
    deadline = time.monotonic() + 10
    while True:
        result = serial.shell('jobs')
        if b'Stopped' in result: break
        if time.monotonic() >= deadline: raise AssertionError('background cat did not receive TTIN')
    serial.shell('kill -KILL "$cat_pid" || exit 1; wait "$cat_pid"; while kill -0 "$cat_pid" 2>/dev/null; do :; done; jobs >/dev/null; printf "TTY_CAT_REAPED\\n"', 'TTY_CAT_REAPED')
    serial.shell("stty tostop || exit 1; printf 'bg-output' & output_pid=$!")
    deadline = time.monotonic() + 10
    while True:
        result = serial.shell('jobs')
        if b'Stopped' in result: break
        if time.monotonic() >= deadline: raise AssertionError('background output did not receive TTOU')
    serial.shell('stty -tostop || exit 1; kill -KILL "$output_pid" || exit 1; wait "$output_pid"; while kill -0 "$output_pid" 2>/dev/null; do :; done; jobs >/dev/null; printf "TTY_OUTPUT_REAPED\\n"', 'TTY_OUTPUT_REAPED')
    serial.shell("printf 'TTY_APPS_PASS\\n'", 'TTY_APPS_PASS')
    serial.send(b'exit 0\n')


def probe(serial):
    while True:
        text = serial.expect(rb'(?m)^(?:TTY_REQUEST [A-Za-z0-9_.-]+ [0-9a-f]*|TTY_PROBE_PASS)\r?\n', 30)
        line = re.findall(rb'(?m)^(TTY_REQUEST [A-Za-z0-9_.-]+ [0-9a-f]*|TTY_PROBE_PASS)\r?$', text)[-1]
        if line == b'TTY_PROBE_PASS': return
        serial.send(bytes.fromhex(line.split(b' ', 2)[2].decode()))


def fixture(directory, launcher, gate, busybox, probe_program, no_ctty=False):
    tree = directory / 'tree'; tree.mkdir()
    (tree / 'bin').mkdir()
    shutil.copy2(launcher, tree / 'init'); shutil.copy2(gate, tree / 'gate')
    shutil.copy2(busybox, tree / 'busybox')
    if probe_program: shutil.copy2(probe_program, tree / 'probe')
    if no_ctty: (tree / 'probe-no-ctty').touch()
    for applet in ('sh', 'stty', 'cat', 'wc', 'sleep', 'printf', 'kill', 'true', 'false', 'test'):
        (tree / 'bin' / applet).symlink_to('/busybox')
    image = directory / 'fixture.img'
    with image.open('wb') as stream: stream.truncate(32 * 1024 * 1024)
    command('mkfs.ext4', '-q', '-F', '-b', '4096', '-d', str(tree), str(image))
    return image


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--arch',choices=('riscv','loongarch'),default='riscv')
    parser.add_argument('--memory',choices=('512M','1G'),action='append')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--linux-kernel', type=Path)
    parser.add_argument('--only', choices=('both', 'linux', 'boaros'), default='both')
    parser.add_argument('--probe', type=Path)
    parser.add_argument('--no-ctty', action='store_true', help='probe establishes its own controlling terminal')
    parser.add_argument('--transport', choices=('legacy', 'modern'), default='modern')
    parser.add_argument('--qemu')
    args = parser.parse_args()
    profile=PROFILES[args.arch];args.kernel=args.kernel or ROOT/profile.kernel;args.qemu=args.qemu or profile.qemu
    if args.arch=='loongarch' and args.transport=='legacy':parser.error('LA platform supports modern PCI')
    if args.no_ctty and not args.probe: parser.error('--no-ctty requires --probe')
    base = ROOT / ('build/riscv' if args.arch=='riscv' else 'build/loongarch'); base.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='tty-run.', dir=base))
    print('TTY artifacts:', directory, flush=True)
    compiler = ROOT / ('build/riscv/musl-root/bin/musl-gcc' if args.arch=='riscv' else 'build/loongarch/musl-root/bin/musl-gcc')
    environment=os.environ.copy()
    if args.arch=='loongarch':environment['REALGCC']=str(ROOT/'build/loongarch/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc')
    launcher = directory / 'launcher'; gate = directory / 'gate'
    flags = (['-fno-link-libatomic'] if args.arch=='riscv' else [*profile.raw_flags,'-Wl,-z,max-page-size=16384'])+['-static', '-O2', '-Wall', '-Wextra', '-Werror']
    command(str(compiler), *flags, str(ROOT / 'tests/tty/launcher.c'), '-o', str(launcher),env=environment)
    command(str(compiler), *flags, str(ROOT / 'tests/tty/gate.c'), '-o', str(gate),env=environment)
    busybox = ROOT / ('build/program-environment/full-busybox/source/busybox/busybox' if args.arch=='riscv' else 'build/loongarch/busybox-source/busybox/busybox')
    disk = fixture(directory, launcher, gate, busybox, args.probe, args.no_ctty)
    variants = []
    if args.only != 'boaros':
        sys.path.insert(0, str(ROOT / 'tests/diff-abi')); import harness
        if args.arch=='loongarch':image=args.linux_kernel or profile.linux_kernel()
        else:image, _identity = harness.fixed_linux_image(args.linux_kernel)
        variants.append(('linux', image))
    if args.only != 'linux': variants.append(('boaros', args.kernel))
    initrd=None
    if args.arch=='loongarch':
        sys.path.insert(0,str(ROOT/'tests/loongarch'))
        from reference import archive
        supervisor=directory/'supervisor'
        command(profile.compiler,*profile.raw_flags,'-O2','-DEXPECTED_EXIT_STATUS=42','-DROOT_PROC_CLEANUP=1',
            '-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdlib','-nostartfiles','-static','-no-pie',
            '-Wl,--build-id=none','-Wl,-z,max-page-size=16384','-T','tests/common/user.ld',
            'tests/loongarch/root_linux_init.c','tests/common/user_start.S','-o',str(supervisor))
        initrd=directory/'initramfs.gz';initrd.write_bytes(archive([('dev',0o040755,b'',0,0),('dev/console',0o020600,b'',5,1),
            ('init',0o100755,supervisor.read_bytes(),0,0),('TRAILER!!!',0,b'',0,0)]))
    records = []
    runs=[(name,kernel,memory) for name,kernel in variants for memory in args.memory or (('512M','1G') if args.arch=='loongarch' else ('512M',))]
    for name, kernel,memory in runs:
        work = directory / (name+'-'+memory); work.mkdir()
        snapshot = work / 'kernel'; shutil.copy2(kernel, snapshot)
        image = work / 'root.img'; shutil.copy2(disk, image)
        invocation=[args.qemu,'-machine','virt','-smp','1','-m',memory,'-kernel',str(snapshot),
            '-display','none','-monitor','none','-serial','stdio','-net','none','-no-reboot','-drive',f'file={image},if=none,format=raw,id=root',
            '-device',profile.block(args.transport)]
        if args.arch=='loongarch':invocation+=['-cpu','la464']
        else:invocation+=['-bios','default','-global','virtio-mmio.force-legacy='+('true' if args.transport=='legacy' else 'false')]
        if name=='linux':invocation+=(['-initrd',str(initrd),'-append','console=ttyS0 rdinit=/init loglevel=3'] if args.arch=='loongarch' else
            ['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1'])
        (work / 'identity.json').write_text(json.dumps({'kernel': digest(snapshot), 'launcher': digest(launcher),
            'gate': digest(gate), 'busybox': digest(busybox), 'fixture': digest(disk),
            'probe': digest(args.probe) if args.probe else None, 'argv': invocation,
            'qemu': command(args.qemu, '--version', capture_output=True, text=True).stdout.splitlines()[0]}, indent=2) + '\n')
        serial = Serial(invocation, work)
        try:
            probe(serial) if args.probe else applications(serial)
            serial.finish()
        finally:
            serial.close()
        result = command('debugfs', '-R', 'cat /tty-status', str(image), capture_output=True, text=True).stdout
        if result.strip() != 'wait_status=0': raise AssertionError(f'{name} child exit: {result!r}')
        if name == 'boaros' and not profile.root_success(bytes(serial.data).decode(errors='replace'),42):
            raise AssertionError('BoarOS TTY root cleanup not complete')
        observed = re.findall(rb'(?m)^TTY_RECORD[^\r\n]*', serial.data)
        records.append(observed)
        if name=='linux' and args.arch=='loongarch' and b'Linux LA root application passed' not in serial.data:
            raise AssertionError('Linux real wait status/root unmount failed')
        print(name,memory, 'TTY PASS', len(observed), 'records', flush=True)
    if args.probe and any(row!=records[0] for row in records[1:]):
        raise AssertionError(f'TTY differential mismatch: {records}')


if __name__ == '__main__':
    main()
