"""Architecture profiles for unchanged GNU runtime consumers."""
import hashlib
import json
import os
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

def checked_inputs(arch='riscv', tools_root=None):
    if arch not in ('riscv','loongarch'):
        raise ValueError('unknown GNU runtime architecture: '+arch)
    manifest=json.loads((HERE/('inputs.json' if arch=='riscv' else 'inputs-loongarch.json')).read_text())
    tools_root=tools_root or os.environ.get('BOAROS_GLIBC_ROOT')
    if tools_root:
        root=Path(tools_root).resolve()
        prefix='/usr' if arch=='riscv' else '/opt/loongarch64-tools'
        def relocated(name):
            path=Path(name)
            relative=path.relative_to(prefix)
            return str(root/('usr' if arch=='riscv' else '')/relative)
        # 只迁移宿主安装位置，原工具、CRT 和 libc 的内容身份继续严格校验。
        for role in ('tools','runtime','installation'):
            manifest[role]={relocated(name):expected for name,expected in manifest.get(role,{}).items()}
        if arch=='riscv':
            target=root/'usr/riscv64-linux-gnu'
            manifest['compiler_flags']=[*manifest.get('compiler_flags',[]),
                '--sysroot='+str(target),'-isystem',str(target/'usr/include'),
                '-isystem',str(target/'include'),'-B'+str(target/'usr/lib/'),
                '-Wl,-rpath-link,'+str(target/'lib')]
    for role in ('tools','runtime'):
        for name,expected in manifest[role].items():
            if not Path(name).is_file() or digest(name)!=expected:raise RuntimeError(f'{role} identity mismatch: {name}')
    for name,expected in manifest.get('installation',{}).items():
        if tree_digest(name)!=expected:raise RuntimeError('runtime installation mismatch: '+name)
    libc=next(Path(p) for p in manifest['runtime'] if p.endswith('/libc.so.6'))
    if f"stable release version {manifest['glibc_version']}".encode() not in libc.read_bytes():
        raise RuntimeError('glibc release version differs from pinned binary')
    return manifest
