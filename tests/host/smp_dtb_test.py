#!/usr/bin/env python3
"""Bounded CPU discovery exercises production DTB parsing and publication."""
import ctypes as C
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class CpuInfo(C.Structure):
    _fields_ = [("hart_ids", C.c_uint64 * 2), ("count", C.c_uint32)]


def blob(cpus, cells=1, parent_status=None):
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
        status, out = run([cpu(19), cpu(7)], boot=19)
        assert status == 0 and out.count == 2 and list(out.hart_ids) == [19, 7]
        status, out = run([cpu(0), (4, b"disabled\0", None, False)])
        assert status == 0 and out.count == 1
        status, out = run([cpu(0x100000007), cpu(0x100000002)],
                          boot=0x100000002, cells=2)
        assert status == 0 and list(out.hart_ids) == [0x100000002, 0x100000007]
        for harts, kwargs in [
            ([cpu(0), cpu(0)], {}), ([cpu(0), cpu(2), cpu(9)], {}),
            ([cpu(3)], {}), ([(0, None, 8, False)], {}),
            ([(0, None, None, True)], {}),
            ([(0, b"broken", None, False)], {}),
            ([cpu(0)], {"parent_status": b"disabled\0"}),
        ]:
            assert run(harts, **kwargs)[0] != 0, (harts, kwargs)
    print("SMP DTB CPU identity, bounds and atomic publication passed")


if __name__ == "__main__":
    main()
