#!/usr/bin/env python3
"""Dedicated fixed-Linux BPF profile; existing reference caches stay untouched."""
import argparse
import json
import os
from pathlib import Path
import shutil
import sys

from build import BASE, ROOT, TARGETS, linux_source, run, sha

ENABLED = ('SMP','BPF','BPF_SYSCALL','BPF_JIT','BPF_EVENTS','BPF_STREAM_PARSER',
    'NET','INET','UNIX','NETFILTER','PERF_EVENTS','FTRACE','FTRACE_SYSCALLS',
    'KPROBES','KPROBE_EVENTS','UPROBES','UPROBE_EVENTS','TRACEPOINTS',
    'DEBUG_KERNEL','DEBUG_INFO','DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT','DEBUG_INFO_BTF',
    'PROC_FS','SYSFS','TMPFS','DEVTMPFS','DEVTMPFS_MOUNT','BINFMT_ELF','BINFMT_SCRIPT',
    'FHANDLE','EPOLL','POSIX_TIMERS','IKCONFIG','IKCONFIG_PROC','RANDOM_TRUST_CPU')

# The stock LA desktop defconfig otherwise builds GPU/storage/network hardware
# and unrelated filesystems for a guest using only virtio-blk and loopback UDP.
DISABLED = ('DRM','SOUND','USB_SUPPORT','SCSI','ATA','MD','INFINIBAND','MEDIA_SUPPORT',
    'INPUT','HID_SUPPORT','ETHERNET','WLAN','WIRELESS','BT','CAN','HAMRADIO',
    'BTRFS_FS','XFS_FS','OCFS2_FS','UBIFS_FS','F2FS_FS','NFS_FS','NFSD','CIFS',
    'SMB_SERVER','CEPH_FS','GFS2_FS','ISO9660_FS','HFS_FS','HFSPLUS_FS','NTFS3_FS',
    'FUSE_FS','OVERLAY_FS','EROFS_FS','SQUASHFS')


def prepare(arch,jobs=2):
    source=linux_source()
    directory=BASE/arch/'linux'; directory.mkdir(parents=True,exist_ok=True)
    command=['make','-C',source,'O='+str(directory),'ARCH='+arch,
             'CROSS_COMPILE='+TARGETS[arch][1],'HOSTCC=gcc',
             'KBUILD_BUILD_USER=boaros','KBUILD_BUILD_HOST=bpf-reference',
             'KBUILD_BUILD_TIMESTAMP=2026-01-01 00:00:00 UTC']
    if not (directory/'.config').is_file():
        if arch=='riscv':
            shutil.copyfile(ROOT/'tests/diff-abi/linux.config',directory/'.config')
            run([*command,'olddefconfig'],directory/'base-configure.log')
        else:
            run([*command,'defconfig'],directory/'base-configure.log')
        options=['--file',str(directory/'.config')]
        for key in ENABLED: options+=['-e',key]
        for key in ('DEBUG_INFO_NONE','DEBUG_INFO_REDUCED','DEBUG_INFO_SPLIT','DEBUG_INFO_BTF_MODULES',
                    'LOCALVERSION_AUTO','MODULES',*DISABLED):
            options+=['-d',key]
        if arch=='loongarch': options+=['-d','4KB_3LEVEL','-d','4KB_4LEVEL','-e','16KB_3LEVEL']
        else: options+=['--set-val','NR_CPUS','8']
        run([source/'scripts/config',*options],directory/'bpf-configure.log')
        run([*command,'olddefconfig'],directory/'configure.log')
    config=(directory/'.config').read_text()
    for key in ('BPF_SYSCALL','BPF_EVENTS','FTRACE_SYSCALLS','DEBUG_INFO_BTF'):
        if 'CONFIG_'+key+'=y' not in config: raise RuntimeError('BPF Linux profile lacks '+key)
    if 'CONFIG_SMP=y' not in config or (arch=='riscv' and 'CONFIG_NR_CPUS=8' not in config):
        raise RuntimeError('BPF Linux profile must support genuine 1/2/8 CPU execution')
    target='Image' if arch=='riscv' else 'vmlinux'
    image=directory/('arch/riscv/boot/Image' if arch=='riscv' else 'vmlinux')
    run([*command,'-j'+str(jobs),target],directory/'build.log')
    identity={'revision': __import__('build').LINUX_REVISION,'arch':arch,
              'configuration_sha256':sha(directory/'.config'),'image_sha256':sha(image),
              'compiler_sha256':sha(TARGETS[arch][1]+'gcc'),'builder_sha256':sha(__file__)}
    (directory/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    print(image)
    return image

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--arch',choices=TARGETS,required=True)
    p.add_argument('--jobs',type=int,default=2);a=p.parse_args();prepare(a.arch,a.jobs)
