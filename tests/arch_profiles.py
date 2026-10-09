"""Immutable target facts shared by native platform acceptance runners."""
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
@dataclass(frozen=True)
class ArchitectureProfile:
    name: str
    page_size: int
    qemu: str
    compiler: str
    raw_flags: tuple
    kernel: str
    def musl_flags(self,compiler):
        if self.name=='loongarch':return [*self.raw_flags,'-Wl,-z,max-page-size=16384']
        # GCC 13 无此新选项；仅支持它的编译器需要禁止隐式链接 libatomic。
        probe=subprocess.run([str(compiler),'-fno-link-libatomic','-E','-x','c','/dev/null'],
                             stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        return ['-fno-link-libatomic'] if probe.returncode==0 else []
    def boot(self,qemu,kernel,memory):
        command=[qemu,'-machine','virt','-smp','1','-m',memory,'-kernel',str(kernel),'-nographic','-no-reboot']
        return command+(['-cpu','la464','-global','ls7a_rtc.toy-enabled=on'] if self.name=='loongarch' else ['-bios','default'])
    def linux_kernel(self):
        if self.name=='loongarch':
            image=ROOT/'build/linux-la-platform/vmlinux'
            identity=json.loads((image.parent/'boaros-identity.json').read_text())
            revision=next(row.split('\t')[4] for row in (ROOT/'references/sources.tsv').read_text().splitlines() if row.startswith('snapshot\tlinux\t'))
            actual=subprocess.check_output(['git','-C',str(ROOT/'references/linux'),'rev-parse','HEAD'],text=True).strip()
            configuration=image.parent/'.config'
            if identity['revision']!=revision or actual!=revision or 'CONFIG_16KB_3LEVEL=y' not in configuration.read_text():
                raise RuntimeError('LA reference Linux revision/page configuration mismatch')
            if hashlib.sha256(configuration.read_bytes()).hexdigest()!=identity.get('configuration_sha256') or hashlib.sha256(image.read_bytes()).hexdigest()!=identity.get('image_sha256'):
                raise RuntimeError('LA reference Linux content mismatch; run prepare-la-linux-platform')
            return image
        sys.path.insert(0,str(ROOT/'tests/diff-abi'))
        import harness
        return harness.linux_build()[0]
    def block(self,transport):
        if self.name=='loongarch':return 'virtio-blk-pci,drive=root,addr=1,disable-legacy=on'
        return 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0'
    def rng(self):
        return ('virtio-rng-pci,rng=entropy,addr=9,disable-legacy=on' if self.name=='loongarch'
                else 'virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.5')
    def root_success(self,text,status):
        if self.name=='loongarch':
            return f'LA PID 1 exited reason=0x0000000000000001 status=0x{status:016x}' in text and 'LA root owners released' in text
        return f'PID 1 exited status=0x{status:x} ' in text and 'heap-live=0x0' in text and 'root finish failure' not in text
PROFILES={
    'riscv':ArchitectureProfile('riscv',4096,'qemu-system-riscv64','riscv64-elf-gcc',
        ('-march=rv64imac_zicsr_zifencei','-mabi=lp64','-mcmodel=medany'),'kernel-rv'),
    'loongarch':ArchitectureProfile('loongarch',16384,'build/qemu-la-rtc/qemu-system-loongarch64',
        'loongarch64-unknown-linux-gnu-gcc',('-mabi=lp64s','-msoft-float','-mno-lsx','-mno-lasx'),'kernel-la')}
