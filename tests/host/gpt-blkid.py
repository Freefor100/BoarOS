#!/usr/bin/env python3
"""Independent Linux util-linux libblkid inspection of the same GPT image."""
import ctypes as c
import ctypes.util
import sys
lib = c.CDLL(ctypes.util.find_library('blkid') or 'libblkid.so.1')
for name, result, args in [
    ('blkid_new_probe_from_filename', c.c_void_p, [c.c_char_p]),
    ('blkid_probe_get_partitions', c.c_void_p, [c.c_void_p]),
    ('blkid_partlist_numof_partitions', c.c_int, [c.c_void_p]),
    ('blkid_partlist_get_partition', c.c_void_p, [c.c_void_p, c.c_int]),
    ('blkid_partition_get_partno', c.c_int, [c.c_void_p]),
    ('blkid_partition_get_start', c.c_longlong, [c.c_void_p]),
    ('blkid_partition_get_size', c.c_longlong, [c.c_void_p]),
    ('blkid_free_probe', None, [c.c_void_p]),
]:
    f = getattr(lib, name); f.restype = result; f.argtypes = args
probe = lib.blkid_new_probe_from_filename(sys.argv[1].encode())
assert probe
try:
    parts = lib.blkid_probe_get_partitions(probe)
    assert parts and lib.blkid_partlist_numof_partitions(parts) == 2
    found = []
    for i in range(2):
        part = lib.blkid_partlist_get_partition(parts, i)
        found.append((lib.blkid_partition_get_partno(part), lib.blkid_partition_get_start(part), lib.blkid_partition_get_size(part)))
    assert found == [(1, 130, 16), (3, 160, 16)], found
    print('libblkid: identical GPT partition slots/start/size', found)
finally:
    lib.blkid_free_probe(probe)
