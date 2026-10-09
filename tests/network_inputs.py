#!/usr/bin/env python3
"""Restore the external runner's original RV runtime, with atomic publication."""
import hashlib
import json
import lzma
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify(path, expected):
    if not path.is_file() or digest(path) != expected:
        raise RuntimeError('original RV network runtime identity mismatch: ' + str(path))


def unpack(archive, image, archive_sha, image_sha):
    verify(archive, archive_sha)
    if image.exists():
        verify(image, image_sha)
        return image
    image.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=image.parent, prefix='.runtime-', delete=False) as target:
        temporary = Path(target.name)
        try:
            with lzma.open(archive, 'rb') as source:
                while block := source.read(1024 * 1024):
                    # 原盘零区保持空洞；逻辑内容与完整镜像SHA仍逐字节一致。
                    if block.count(0) == len(block):
                        target.seek(len(block), os.SEEK_CUR)
                    else:
                        target.write(block)
            target.truncate()
            target.flush()
            os.fsync(target.fileno())
            verify(temporary, image_sha)
            temporary.replace(image)
        finally:
            temporary.unlink(missing_ok=True)
    return image


def prepare():
    inputs = json.loads((ROOT / 'tests/workloads/network/inputs.json').read_text())
    rows = [line.split('\t') for line in (ROOT / 'references/sources.tsv').read_text().splitlines()
            if line and not line.startswith('#')]
    selected = [row for row in rows if row[1] == inputs['archive_reference']]
    if len(selected) != 1 or selected[0][0] != 'file':
        raise RuntimeError('missing or ambiguous original RV network archive reference')
    row = selected[0]
    image = ROOT / 'build/tools/network-rv/sdcard-rv.img'
    if image.exists():
        verify(image, inputs['image_sha256'])
        return image
    # Harness原盘属于另一owner；只有匹配真实解压身份才借用，不搬动或修补它。
    old = ROOT / 'references/oscomp-autotest/sdcard-rv.img'
    if old.exists():
        if digest(old) == inputs['image_sha256']:
            return old
        print('Existing Harness RV image differs from the pinned archive; leaving it unchanged and restoring the verified network input', file=sys.stderr)
    archive = ROOT / 'references' / row[1]
    old_archive = old.with_suffix('.img.xz')
    if not archive.exists() and old_archive.exists():
        verify(old_archive, row[4])
        archive = old_archive
    if not archive.exists():
        image.parent.mkdir(parents=True, exist_ok=True)
        manifest = image.parent / 'references.tsv'
        manifest.write_text('\t'.join(row) + '\n')
        subprocess.run(['sh', str(ROOT / 'references/fetch.sh'), '--manifest', str(manifest)],
                       cwd=ROOT, check=True)
    return unpack(archive, image, row[4], inputs['image_sha256'])


if __name__ == '__main__':
    image = prepare()
    print('Original RV network runtime verified:', image)
