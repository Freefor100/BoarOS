"""Locate the original disks shared by the repository's worktrees."""
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def directory(profile):
    git_dir = subprocess.run(
        ['git', 'rev-parse', '--path-format=absolute', '--git-common-dir'],
        cwd=ROOT, capture_output=True, text=True,
    )
    root = Path(git_dir.stdout.strip()).parent if git_dir.returncode == 0 else ROOT
    return root / 'sdcard' / profile


def preliminary_image(arch):
    name = 'sdcard-' + {'riscv': 'rv', 'loongarch': 'la'}[arch] + '.img'
    image = directory('pre-20250615') / name
    if not image.is_file():
        raise FileNotFoundError(f'Extract the release disk to {image}')
    return image
