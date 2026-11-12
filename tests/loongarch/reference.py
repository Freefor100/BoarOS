#!/usr/bin/env python3
"""Build reproducible initramfs archives for the fixed Linux baseline."""
import gzip

REVISION = 'f4cdf7ca9a1fdcca413157df19753f388a5a224e'


def archive(entries):
    output = bytearray()
    for inode, (name, mode, data, major, minor) in enumerate(entries, 1):
        encoded = name.encode() + b'\0'
        values = (inode, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(encoded), 0)
        output.extend(b'070701' + ''.join(f'{v:08x}' for v in values).encode())
        output.extend(encoded)
        output.extend(b'\0' * (-len(output) % 4))
        output.extend(data)
        output.extend(b'\0' * (-len(output) % 4))
    return gzip.compress(bytes(output), mtime=0)
