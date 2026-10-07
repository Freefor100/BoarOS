#!/usr/bin/env python3
"""Build reusable LA tools from the repository's fixed reference inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
QEMU_OPTIONS = ['--target-list=loongarch64-softmmu', '--disable-docs',
                '--disable-tools', '--disable-guest-agent', '--disable-werror']


def capture(command):
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()


def fixed_repository(name):
    entries = [line.split('\t') for line in
               (ROOT / 'references/sources.tsv').read_text().splitlines()
               if line and not line.startswith('#')]
    revision = next(entry[4] for entry in entries if entry[1] == name)
    source = ROOT / 'references' / name
    if capture(['git', '-C', str(source), 'rev-parse', 'HEAD']) != revision:
        raise SystemExit(f'{source}: revision differs from sources.tsv')
    if capture(['git', '-C', str(source), 'status', '--porcelain']):
        raise SystemExit(f'{source}: reference has local modifications')
    return source, revision


def compiler_identity(compiler):
    found = shutil.which(compiler)
    if not found:
        raise SystemExit(f'missing compiler: {compiler}')
    path = Path(found).resolve()
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'version': capture([str(path), '--version']).splitlines()[0],
            'target': capture([str(path), '-dumpmachine'])}


def prepare(args):
    source, revision = fixed_repository('qemu' if args.component == 'tools' else 'linux')
    profile=getattr(args,'profile','core')
    directory = ROOT / 'build' / ('qemu-la' if args.component == 'tools' else
        'linux-la-platform' if profile=='platform' else 'linux-la')
    directory.mkdir(parents=True, exist_ok=True)
    stamp = directory / 'boaros-identity.json'
    compiler = compiler_identity('cc' if args.component == 'tools' else args.cross + 'gcc')
    identity = {'revision': revision, 'compiler': compiler, 'profile':
                QEMU_OPTIONS if args.component == 'tools' else
                'la64-16kb-3level-virt-platform-v1' if profile=='platform' else 'la64-16kb-3level-initramfs-v1'}
    recorded = json.loads(stamp.read_text()) if stamp.exists() else None
    if recorded and {key: value for key, value in recorded.items()
                     if key not in ('configuration_sha256','image_sha256')} != identity:
        raise SystemExit(f'{directory}: cache identity changed; use a fresh build directory')
    if args.component == 'tools':
        if recorded is None or not (directory / 'build.ninja').exists():
            subprocess.run([str(source / 'configure'), *QEMU_OPTIONS], cwd=directory, check=True)
        subprocess.run(['ninja', '-C', str(directory), f'-j{args.jobs}',
                        'qemu-system-loongarch64'], check=True)
        print(capture([str(directory / 'qemu-system-loongarch64'), '--version']))
    else:
        if not compiler['target'].startswith('loongarch64'):
            raise SystemExit('Linux LA requires a LoongArch64 compiler')
        command = ['make', '-C', str(source), f'O={directory}', 'ARCH=loongarch',
                   f'CROSS_COMPILE={compiler["path"][:-3]}']
        configuration = directory / '.config'
        if recorded and 'configuration_sha256' in recorded:
            if not configuration.exists() or hashlib.sha256(configuration.read_bytes()).hexdigest() != recorded['configuration_sha256']:
                raise SystemExit(f'{directory}: Linux configuration differs from cache identity')
        if recorded is None or not (directory / '.config').exists():
            subprocess.run([*command, 'defconfig'], check=True)
            subprocess.run([str(source / 'scripts/config'), '--file', str(directory / '.config'),
                            '-e', 'BLK_DEV_INITRD', '-e', 'RD_GZIP', '-e', 'DEVTMPFS',
                            '-e', 'BINFMT_ELF', '-d', '4KB_3LEVEL', '-d', '4KB_4LEVEL',
                            '-e', '16KB_3LEVEL'], check=True)
            if profile=='platform':
                subprocess.run([str(source/'scripts/config'),'--file',str(configuration),
                    '-e','HW_RANDOM_VIRTIO','-e','VIRTIO_NET','-e','NET_FAILOVER','-e','FAILOVER',
                    '-e','RTC_DRV_LOONGSON'],check=True)
            subprocess.run([*command, 'olddefconfig'], check=True)
        if 'CONFIG_16KB_3LEVEL=y' not in (directory / '.config').read_text():
            raise SystemExit('Linux LA cache no longer uses 16 KiB, three levels')
        if profile=='platform' and any('CONFIG_'+symbol+'=y' not in configuration.read_text()
            for symbol in ('HW_RANDOM_VIRTIO','VIRTIO_NET','NET_FAILOVER','FAILOVER','RTC_DRV_LOONGSON')):
            raise SystemExit('Linux LA platform cache lacks builtin devices')
        subprocess.run([*command, f'-j{args.jobs}', 'vmlinux'], check=True)
        identity['configuration_sha256'] = hashlib.sha256(configuration.read_bytes()).hexdigest()
    identity['image_sha256']=hashlib.sha256((directory/('qemu-system-loongarch64' if args.component=='tools' else 'vmlinux')).read_bytes()).hexdigest()
    stamp.write_text(json.dumps(identity, indent=2) + '\n')
    print(f'LA {args.component} cache: {directory}; source {revision}; {compiler["version"]}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component', choices=('tools', 'linux'), required=True)
    parser.add_argument('--cross', default='loongarch64-unknown-linux-gnu-')
    parser.add_argument('--profile',choices=('core','platform'),default='core')
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 1, 12))
    prepare(parser.parse_args())
