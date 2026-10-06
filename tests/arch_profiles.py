"""Immutable target facts shared by native platform acceptance runners."""
from dataclasses import dataclass
from pathlib import Path
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
    def boot(self,qemu,kernel,memory):
        command=[qemu,'-machine','virt','-smp','1','-m',memory,'-kernel',str(kernel),'-nographic','-no-reboot']
        return command+(['-cpu','la464'] if self.name=='loongarch' else ['-bios','default'])
    def linux_kernel(self):
        if self.name=='loongarch':return ROOT/'build/linux-la-platform/vmlinux'
        sys.path.insert(0,str(ROOT/'tests/diff-abi'))
        import harness
        key,_=harness.identity(ROOT/'tests/diff-abi/linux.config')
        return ROOT/'build/diff-abi/linux'/key/'vmlinux'
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
    'loongarch':ArchitectureProfile('loongarch',16384,'build/qemu-la/qemu-system-loongarch64',
        'loongarch64-unknown-linux-gnu-gcc',('-mabi=lp64s','-msoft-float','-mno-lsx','-mno-lasx'),'kernel-la')}
