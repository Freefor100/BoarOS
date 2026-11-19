#!/usr/bin/env python3
"""Bounded CPU discovery exercises production DTB parsing and publication."""
import ctypes as C
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class CpuInfo(C.Structure):
    _fields_ = [("hart_ids", C.c_uint64 * 8), ("count", C.c_uint32), ("features", C.c_uint32)]


def blob(cpus, cells=1, parent_status=None, extensions=None, isa=None, zero_blocks=None):
    strings, body, offsets = bytearray(), bytearray(), {}
    word = lambda value: struct.pack(">I", value)

    def begin(name):
        body.extend(word(1) + name.encode() + b"\0")
        body.extend(b"\0" * (-len(body) % 4))

    def end():
        body.extend(word(2))

    def prop(name, value):
        if name not in offsets:
            offsets[name] = len(strings)
            strings.extend(name.encode() + b"\0")
        body.extend(word(3) + word(len(value)) + word(offsets[name]) + value)
        body.extend(b"\0" * (-len(body) % 4))

    begin("")
    prop("#address-cells", word(2))
    prop("#size-cells", word(2))
    begin("memory@80000000")
    prop("device_type", b"memory\0")
    prop("reg", struct.pack(">QQ", 0x80000000, 0x20000000))
    end()
    begin("cpus")
    prop("#address-cells", word(cells))
    prop("#size-cells", word(0))
    if parent_status:
        prop("status", parent_status)
    for index, (hart, status, reg_size, duplicate) in enumerate(cpus):
        begin(f"cpu@{index:x}")
        prop("device_type", b"cpu\0")
        value = hart.to_bytes(4 * cells, "big") if reg_size is None else b"\0" * reg_size
        prop("reg", value)
        if duplicate:
            prop("reg", value)
        if extensions and hart in extensions:
            prop("riscv,isa-extensions", extensions[hart])
        if isa and hart in isa:
            prop("riscv,isa", isa[hart])
        if zero_blocks and hart in zero_blocks:
            prop("riscv,cboz-block-size", word(zero_blocks[hart]))
        if status:
            prop("status", status)
        end()
    begin("cpu-map")
    begin("cluster0")
    end()
    end()
    end()
    end()
    body.extend(word(9))
    header = struct.pack(">10I", 0xD00DFEED, 56 + len(body) + len(strings),
                         56, 56 + len(body), 40, 17, 16, 0, len(strings), len(body))
    return header + b"\0" * 16 + body + strings


def main():
    with tempfile.TemporaryDirectory() as tmp:
        lib = Path(tmp) / "dtb.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "include"), str(ROOT / "kernel/dtb.c"),
                        "-o", str(lib)], check=True)
        parse = C.CDLL(str(lib)).dtb_read_cpu_info
        parse.argtypes = [C.c_void_p, C.c_uint64, C.POINTER(CpuInfo)]

        def run(harts, boot=0, **kwargs):
            data = C.create_string_buffer(blob(harts, **kwargs))
            out = CpuInfo()
            C.memset(C.byref(out), 0x55, C.sizeof(out))
            before = bytes(out)
            status = parse(data, boot, C.byref(out))
            if status:
                assert bytes(out) == before, "failed enumeration published a partial CPU list"
            return status, out

        cpu = lambda hart: (hart, None, None, False)
        feature=b"i\0svvptc\0"
        status, out=run([cpu(0),cpu(1)],extensions={0:feature,1:feature})
        assert status==0 and out.features==1, "Svvptc on every enabled hart must be published"
        assert run([cpu(0),cpu(1)],extensions={0:feature})[1].features==0
        assert run([cpu(0),(1,b"disabled\0",None,False)],extensions={0:feature})[1].features==1
        assert run([cpu(0)],isa={0:b"rv64imac_svvptc\0"})[1].features==1
        assert run([cpu(0)],isa={0:b"rv64imac_svvptc1p0\0"})[1].features==1
        assert run([cpu(0)],isa={0:b"rv64imac_svvptc1\0"})[1].features==1
        assert run([cpu(0)],isa={0:b"rv64imacsvvptc\0"})[1].features==1
        assert run([cpu(0)],isa={0:b"rv64i2p1m2p0ac_svvptc1p0\0"})[1].features==1
        assert run([cpu(0)],isa={0:b"rv64imac_svvptcbogus\0"})[1].features==0
        assert run([cpu(0)],isa={0:b"rv64imac_xsvvptc\0"})[1].features==0
        assert run([cpu(0)],extensions={0:b"i\0"},isa={0:b"rv64imac_svvptc\0"})[1].features==0
        assert run([cpu(0)],extensions={0:b"i\0svvptc"})[0]!=0
        for text in [b"rv64imafdcv\0",b"rv64i2p1m2p0a2p1f2p2d2p2cv1p0\0"]:
            assert run([cpu(0)],isa={0:text})[1].features==2
        for text in [b"rv64imafdc_xvendor\0",b"rv64imafdcv0p7\0",b"rv64imafdc_zve64d\0"]:
            assert run([cpu(0)],isa={0:text})[1].features==0
        both=b"i\0v\0svvptc\0"
        assert run([cpu(0),cpu(1)],extensions={0:both,1:both})[1].features==3
        assert run([cpu(0),cpu(1)],extensions={0:both,1:feature})[1].features==1
        assert run([cpu(0),(1,b"disabled\0",None,False)],extensions={0:both})[1].features==3
        assert run([cpu(0)],extensions={0:feature},isa={0:b"rv64imafdcv\0"})[1].features==1
        cboz = {0:b"i\0zicboz\0", 1:b"i\0zicboz\0"}
        assert run([cpu(0), cpu(1)], extensions=cboz, zero_blocks={0:64, 1:64})[1].features == 4
        assert run([cpu(0), cpu(1)], extensions=cboz, zero_blocks={0:64, 1:128})[1].features == 0
        assert run([cpu(0)], extensions={0:b"i\0"}, zero_blocks={0:64})[1].features == 0
        assert run([cpu(0)], extensions=cboz)[1].features == 0
        boot_features=C.CDLL(str(lib)).dtb_read_boot_cpu_features
        boot_features.argtypes=[C.c_void_p,C.c_uint64,C.POINTER(C.c_uint32)]
        data=C.create_string_buffer(blob([cpu(i) for i in range(10)],extensions={3:feature}))
        features=C.c_uint32(0x55555555)
        assert boot_features(data,3,C.byref(features))==0 and features.value==1
        assert boot_features(data,5,C.byref(features))==0 and features.value==0
        features.value=0x55555555
        assert boot_features(data,19,C.byref(features))!=0 and features.value==0x55555555

        status, out = run([cpu(19), cpu(7), cpu(63), cpu(2)], boot=19)
        assert status == 0 and out.count == 4 and list(out.hart_ids) == [19, 2, 7, 63, 0, 0, 0, 0]
        status, out = run([cpu(hart) for hart in (19, 7, 63, 2, 8, 91, 4, 5)], boot=19)
        assert status == 0 and out.count == 8 and list(out.hart_ids) == [19, 2, 4, 5, 7, 8, 63, 91]
        status, out = run([cpu(19), cpu(7)], boot=19)
        assert status == 0 and out.count == 2 and list(out.hart_ids) == [19, 7, 0, 0, 0, 0, 0, 0]
        status, out = run([cpu(0), (4, b"disabled\0", None, False)])
        assert status == 0 and out.count == 1
        status, out = run([cpu(0x100000007), cpu(0x100000002)],
                          boot=0x100000002, cells=2)
        assert status == 0 and list(out.hart_ids) == [0x100000002, 0x100000007, 0, 0, 0, 0, 0, 0]
        for harts, kwargs in [
            ([cpu(0), cpu(0)], {}), ([cpu(hart) for hart in range(9)], {}),
            ([cpu(3)], {}), ([(0, None, 8, False)], {}),
            ([(0, None, None, True)], {}),
            ([(0, b"broken", None, False)], {}),
            ([cpu(0)], {"parent_status": b"disabled\0"}),
        ]:
            assert run(harts, **kwargs)[0] != 0, (harts, kwargs)
    print("SMP DTB CPU identity, bounds and atomic publication passed")


if __name__ == "__main__":
    main()
