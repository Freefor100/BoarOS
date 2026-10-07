"""Architecture profiles for unchanged GNU runtime consumers."""
import hashlib
import json
from pathlib import Path
import stat

HERE=Path(__file__).resolve().parent

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def tree_digest(root):
    root=Path(root)
    if not root.is_dir() or root.is_symlink():raise RuntimeError('invalid runtime tree: '+str(root))
    rows=[]
    for path in [root,*sorted(root.rglob('*'))]:
        row=[str(path.relative_to(root)),stat.S_IMODE(path.lstat().st_mode)]
        if path.is_symlink():row+=['link',str(path.readlink())]
        elif path.is_file():row+=['file',digest(path)]
        elif path.is_dir():row+=['directory']
        else:raise RuntimeError('unsupported runtime object: '+str(path))
        rows.append(row)
    return hashlib.sha256(json.dumps(rows,separators=(',',':')).encode()).hexdigest()

def checked_inputs(arch='riscv'):
    manifest=json.loads((HERE/('inputs.json' if arch=='riscv' else 'inputs-loongarch.json')).read_text())
    for role in ('tools','runtime'):
        for name,expected in manifest[role].items():
            if not Path(name).is_file() or digest(name)!=expected:raise RuntimeError(f'{role} identity mismatch: {name}')
    for name,expected in manifest.get('installation',{}).items():
        if tree_digest(name)!=expected:raise RuntimeError('runtime installation mismatch: '+name)
    libc=next(Path(p) for p in manifest['runtime'] if p.endswith('/libc.so.6'))
    if f"stable release version {manifest['glibc_version']}".encode() not in libc.read_bytes():
        raise RuntimeError('glibc release version differs from pinned binary')
    return manifest
