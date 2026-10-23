#!/usr/bin/env python3
"""Add numeric-owner/set-ID/filecap samples to an existing private ext4 fixture."""
import argparse
from pathlib import Path
import re
import struct
import subprocess


def install(image, program, preload, work):
    image, program, preload, work = map(lambda p: Path(p).resolve(), (image, program, preload, work))
    work.mkdir(parents=True, exist_ok=True)
    variants = {'ordinary': (0o100755, 0, 0), 'suid-root': (0o104755, 0, 0),
                'suid-user': (0o104755, 1001, 0), 'sgid': (0o102755, 0, 2001),
                'sgid-no-group-exec': (0o102645, 0, 2001)}
    variants.update({name: (0o100755, 0, 0) for name in
                     ('cap1', 'cap2', 'cap3', 'cap-external', 'cap-rootid', 'cap-zero', 'cap-bad')})
    variants.update({name: (0o106777, 1000, 1000) for name in
                     ('limit-write', 'limit-truncate', 'limit-fallocate')})
    commands = ['mkdir /cred', 'set_inode_field /cred mode 040755', 'mkdir /cred/untrusted']
    for name, (mode, uid, gid) in variants.items():
        commands += [f'write "{program}" /cred/{name}', f'set_inode_field /cred/{name} mode 0{mode:o}',
                     f'set_inode_field /cred/{name} uid {uid}', f'set_inode_field /cred/{name} gid {gid}']
    commands += [f'write "{preload}" /cred/preload.so', 'set_inode_field /cred/preload.so mode 0100755']
    payloads = {'cap1': struct.pack('<III', 0x01000001, 1 << 10, 0),
                'cap2': struct.pack('<IIIII', 0x02000001, 1 << 10, 0, 0, 0),
                'cap3': struct.pack('<IIIIII', 0x03000001, 1 << 10, 0, 0, 0, 0),
                'cap-rootid': struct.pack('<IIIIII', 0x03000001, 1 << 10, 0, 0, 0, 1000),
                'cap-zero': struct.pack('<IIIII', 0x02000000, 0, 0, 0, 0),
                'cap-bad': struct.pack('<II', 0x02000001, 1 << 10)}
    payloads['cap-external'] = payloads['cap2']
    for name in ('limit-write', 'limit-truncate', 'limit-fallocate'):
        payloads[name] = payloads['cap2']
    padding = work / 'inline-padding'; padding.write_bytes(bytes(range(64)))
    commands += [f'ea_set -f "{padding}" /cred/cap-external user.pad']
    for name, data in payloads.items():
        source = work / (name + '.bin'); source.write_bytes(data)
        commands += [f'ea_set -f "{source}" /cred/{name} security.capability']
    commands += ['stat /cred/cap2', 'stat /cred/cap-external']
    batch = work / 'credentials.debugfs'; batch.write_text('\n'.join(commands) + '\n')
    result = subprocess.run(['debugfs', '-w', '-f', str(batch), str(image)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
    (work / 'credentials.debugfs.log').write_text(result.stdout)
    for name, data in payloads.items():
        exported = work / (name + '.actual')
        result = subprocess.run(['debugfs', '-R', f'ea_get -f "{exported}" /cred/{name} security.capability', str(image)],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
        if not exported.is_file() or exported.read_bytes() != data:
            raise RuntimeError('file capability fixture differs: ' + name + '\n' + result.stdout)
    stat = subprocess.check_output(['debugfs', '-R', 'stat /cred/cap-external', str(image)], text=True, stderr=subprocess.STDOUT)
    if not re.search(r'File ACL: [1-9][0-9]*', stat):
        raise RuntimeError('external EA block was not created')
    return image


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--program', required=True, type=Path)
    parser.add_argument('--preload', required=True, type=Path)
    parser.add_argument('--work', required=True, type=Path)
    args = parser.parse_args()
    install(args.image, args.program, args.preload, args.work)
