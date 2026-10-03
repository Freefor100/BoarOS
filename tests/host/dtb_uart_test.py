#!/usr/bin/env python3
"""Exercise real DTB discovery with a generated FDT, no QEMU dependency."""
import ctypes as C
import struct
import subprocess
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
class Range(C.Structure):
    _fields_ = [('base', C.c_uint64), ('size', C.c_uint64)]
class Route(C.Structure):
    _fields_ = [('base', C.c_uint64), ('source', C.c_uint32)]
class Uart(C.Structure):
    _fields_ = [('registers', Range), ('clock', C.c_uint32), ('source', C.c_uint32),
                ('shift', C.c_uint32), ('width', C.c_uint32)]
class Irq(C.Structure):
    _fields_ = [('plic', Range), ('context', C.c_uint32), ('source_count', C.c_uint32),
                ('route_count', C.c_uint32), ('routes', Route * 16), ('uart', Uart)]
def blob(source=11, clock=3686400, shift=0, width=1, size=4096, parent=2, duplicate=False):
    strings = bytearray(); offsets = {}; body = bytearray()
    def word(n): return struct.pack('>I', n)
    def begin(name):
        body.extend(word(1) + name.encode() + b'\0')
        body.extend(b'\0' * (-len(body) % 4))
    def end(): body.extend(word(2))
    def prop(name, val):
        if name not in offsets:
            offsets[name] = len(strings); strings.extend(name.encode() + b'\0')
        body.extend(word(3) + word(len(val)) + word(offsets[name]) + val)
        body.extend(b'\0' * (-len(body) % 4))
    def cell(name, val): prop(name, word(val))
    begin(''); cell('#address-cells',2); cell('#size-cells',2)
    begin('memory@80000000'); prop('device_type', b'memory\0')
    prop('reg',struct.pack('>QQ',0x80000000,0x8000000)); end()
    begin('cpus'); cell('#address-cells',1); cell('#size-cells',0)
    begin('cpu@0'); prop('device_type',b'cpu\0'); cell('reg',0)
    begin('interrupt-controller'); prop('compatible',b'riscv,cpu-intc\0')
    cell('phandle',1); cell('#interrupt-cells',1); end(); end(); end()
    begin('soc'); cell('#address-cells',2); cell('#size-cells',2); prop('ranges',b'')
    begin('plic@c000000'); prop('compatible',b'riscv,plic0\0')
    prop('reg',struct.pack('>QQ',0xc000000,0x4000000)); cell('phandle',2)
    cell('riscv,ndev',31); cell('#interrupt-cells',1)
    prop('interrupts-extended',struct.pack('>IIII',1,11,1,9)); end()
    for i in range(2 if duplicate else 1):
        begin(f'uart@{0x10000000+i*4096:x}'); prop('compatible',b'ns16550a\0')
        prop('reg',struct.pack('>QQ',0x10000000+i*4096,size)); cell('interrupt-parent',parent)
        cell('interrupts',source)
        if clock is not None: cell('clock-frequency',clock)
        cell('reg-shift',shift); cell('reg-io-width',width); end()
    end(); end(); body.extend(word(9))
    header = struct.pack('>10I',0xd00dfeed,56+len(body)+len(strings),56,56+len(body),40,17,16,0,len(strings),len(body))
    return header+b'\0'*16+body+strings
with tempfile.TemporaryDirectory() as tmp:
    lib = Path(tmp)/'dtb.so'
    subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),str(ROOT/'kernel/dtb.c'),'-o',str(lib)],check=True)
    parse=C.CDLL(str(lib)).dtb_read_irq_info
    parse.argtypes=[C.c_void_p,C.c_uint64,C.POINTER(Irq)]
    def run(**args):
        b=C.create_string_buffer(blob(**args)); out=Irq(); return parse(b,0,C.byref(out)),out
    status,out=run(); assert status==0
    assert out.uart.registers.base==0x10000000, 'UART missing from IRQ discovery'
    assert (out.uart.clock,out.uart.source,out.uart.shift,out.uart.width)==(3686400,11,0,1)
    status,out=run(shift=2,width=4); assert status==0 and out.uart.shift==2 and out.uart.width==4
    for args in [dict(clock=None),dict(clock=0),dict(source=0),dict(source=32),dict(parent=3),dict(shift=5),dict(width=2),dict(size=7),dict(duplicate=True)]:
        assert run(**args)[0]!=0, f'accepted malformed UART {args}'
print('DTB UART route/layout tests passed')
