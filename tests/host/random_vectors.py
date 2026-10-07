#!/usr/bin/env python3
"""Compare portable BLAKE2s with an independent standard-library implementation."""
import ctypes
import hashlib
import pathlib
import subprocess
import tempfile
root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='boaros-random-') as temp:
    shared = pathlib.Path(temp) / 'random.so'
    subprocess.run(['cc', '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-DBOAROS_PAGE_SHIFT=12', '-Itests/host/random', '-Iinclude', 'tests/host/random_stubs.c',
                    'kernel/random.c', 'kernel/blake2s.c', '-o', str(shared)],
                   cwd=root, check=True)
    lib = ctypes.CDLL(str(shared))
    lib.kernel_blake2s.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    for size in [0, 1, 3, 31, 32, 63, 64, 65, 127, 128, 129, 1024]:
        data = bytes(i % 251 for i in range(size))
        result = ctypes.create_string_buffer(32)
        lib.kernel_blake2s(data, len(data), result)
        assert result.raw == hashlib.blake2s(data).digest(), size
    executable = pathlib.Path(temp) / 'random-test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-DBOAROS_PAGE_SHIFT=12', '-Itests/host/random', '-Iinclude', 'tests/host/random_test.c',
                    'tests/host/random_stubs.c', 'kernel/random.c', 'kernel/blake2s.c',
                    '-o', str(executable)], cwd=root, check=True)
    subprocess.run([str(executable)], check=True)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-DBOAROS_PAGE_SHIFT=12', '-Itests/host/random', '-Iinclude', 'tests/host/random_device_test.c',
                    'tests/host/random_stubs.c', 'kernel/random.c', 'kernel/blake2s.c',
                    'fs/char_device.c', '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', '-o', str(executable)], cwd=root, check=True)
    subprocess.run([str(executable)], check=True)
print('BLAKE2s independent vectors passed')
