#!/usr/bin/env python3
"""Explicitly restore the pinned Harness and original release assets for a new host."""
import hashlib
import json
import lzma
from pathlib import Path
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''): value.update(block)
    return value.hexdigest()


def verify(path, expected):
    if path.is_symlink() or not path.is_file() or digest(path) != expected:
        raise RuntimeError('fixed asset is missing or altered: ' + str(path))


def main():
    inputs = json.loads((HERE / 'inputs.json').read_text())
    cache = ROOT / 'build/tools/oscomp'; cache.mkdir(parents=True, exist_ok=True)
    manifest = cache / 'references.tsv'
    rows = [line for line in (ROOT / 'references/sources.tsv').read_text().splitlines()
            if line.split('\t')[1:2] == ['oscomp-autotest']]
    if len(rows) != 1: raise RuntimeError('fixed Harness reference is ambiguous')
    manifest.write_text(rows[0] + '\n')
    reference = ROOT / 'references/oscomp-autotest'
    def exclude_assets():
        if not (reference / '.git').exists(): return
        destination = reference / '.git/info/exclude'
        value = destination.read_text() if destination.exists() else ''
        for name in inputs['assets']:
            if '/' + name not in value.splitlines(): value += '\n/' + name + '\n'
        destination.write_text(value)
    exclude_assets()
    subprocess.run(['sh', 'references/fetch.sh', '--manifest', str(manifest), '--root', 'references'], cwd=ROOT, check=True)
    exclude_assets()
    base = inputs['release_url'].replace('/releases/tag/', '/releases/download/')
    for arch in ('rv', 'la'):
        compressed = reference / ('sdcard-' + arch + '.img.xz')
        if not compressed.exists():
            temporary = cache / (compressed.name + '.download')
            print('Downloading pinned asset:', compressed.name, flush=True)
            try:
                with urllib.request.urlopen(base + '/' + compressed.name, timeout=60) as source, temporary.open('wb') as target:
                    while block := source.read(1024 * 1024): target.write(block)
                verify(temporary, inputs['assets'][compressed.name])
                temporary.replace(compressed)
            finally:
                temporary.unlink(missing_ok=True)
        verify(compressed, inputs['assets'][compressed.name])
        image = reference / ('sdcard-' + arch + '.img')
        if not image.exists():
            temporary = cache / (image.name + '.unpack')
            try:
                with lzma.open(compressed, 'rb') as source, temporary.open('wb') as target:
                    while block := source.read(1024 * 1024): target.write(block)
                verify(temporary, inputs['assets'][image.name])
                temporary.replace(image)
            finally:
                temporary.unlink(missing_ok=True)
        verify(image, inputs['assets'][image.name])
        print('Original image verified:', image.name, flush=True)


if __name__ == '__main__':
    main()
