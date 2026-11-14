#!/usr/bin/env python3
"""Compare portable BLAKE2s with an independent standard-library implementation."""
from concurrent.futures import ThreadPoolExecutor
from threading import Barrier
import ctypes
import hashlib
import pathlib
import shutil
import subprocess
import tempfile
root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='boaros-random-') as temp:
    shared = pathlib.Path(temp) / 'random.so'
    subprocess.run(['cc', '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-DBOAROS_PAGE_SHIFT=12', '-Itests/host/random', '-Iinclude', 'tests/host/cpu_context.c', 'tests/host/random_stubs.c',
                    'kernel/random.c', 'kernel/blake2s.c', '-o', str(shared)],
                   cwd=root, check=True)
    lib = ctypes.CDLL(str(shared))
    lib.kernel_blake2s.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    for size in [0, 1, 3, 31, 32, 63, 64, 65, 127, 128, 129, 1024]:
        data = bytes(i % 251 for i in range(size))
        result = ctypes.create_string_buffer(32)
        lib.kernel_blake2s(data, len(data), result)
        assert result.raw == hashlib.blake2s(data).digest(), size
    # Simultaneous execs must take successive keys from the shared stream.
    # A second loaded image supplies the exact serial sequence from the same seed.
    parallel = pathlib.Path(temp) / 'parallel.so'
    subprocess.run(['cc', '-shared', '-fPIC', '-std=gnu11', '-O2',
                    '-DBOAROS_MAX_CPUS=2', '-DBOAROS_PAGE_SHIFT=12',
                    '-Itests/host/random', '-Iinclude', 'kernel/random.c', 'kernel/blake2s.c',
                    'kernel/raw_lock.c', 'tests/host/cpu_context.c', 'tests/host/random_stubs.c',
                    '-o', str(parallel)], cwd=root, check=True)
    serial = pathlib.Path(temp) / 'serial.so'
    shutil.copyfile(parallel, serial)
    run = ctypes.CDLL(str(parallel))
    reference = ctypes.CDLL(str(serial))
    def draw(rng):
        data = ctypes.create_string_buffer(32)
        assert rng.kernel_random_fill(data, len(data)) == 0
        return data.raw
    barrier = Barrier(4)
    def actor(_):
        barrier.wait()
        return [draw(run) for _ in range(64)]
    with ThreadPoolExecutor(max_workers=4) as workers:
        batches = list(workers.map(actor, range(4)))
    assert sorted(v for batch in batches for v in batch) == sorted(draw(reference) for _ in range(256))
    executable = pathlib.Path(temp) / 'random-test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-DBOAROS_PAGE_SHIFT=12', '-Itests/host/random', '-Iinclude', 'tests/host/random_test.c',
                    'tests/host/cpu_context.c', 'tests/host/random_stubs.c', 'kernel/random.c', 'kernel/blake2s.c',
                    '-o', str(executable)], cwd=root, check=True)
    subprocess.run([str(executable)], check=True)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-DBOAROS_PAGE_SHIFT=12', '-Itests/host/random', '-Iinclude', 'tests/host/random_device_test.c',
                    'tests/host/cpu_context.c', 'tests/host/random_stubs.c', 'kernel/random.c', 'kernel/blake2s.c',
                    'fs/char_device.c', 'kernel/credentials.c', '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', '-o', str(executable)], cwd=root, check=True)
    subprocess.run([str(executable)], check=True)
    for shift in (12, 14):
        subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        f'-DBOAROS_PAGE_SHIFT={shift}', '-idirafter', 'include',
                        '-fsanitize=address,undefined', 'tests/host/random_copy.c',
                        '-o', str(executable)], cwd=root, check=True)
        subprocess.run([str(executable)], check=True)
print('BLAKE2s independent vectors passed')
