"""Shared build provenance and exclusion rules for local budget experiments."""
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

SCHEMA = 1
SECTION = '.boaros.experiment'
CANONICAL = re.compile(r'(?:w[0-9]+-p[0-9]+-m[0-9]+|ra[0-9]+-wb[0-9]+)')
EXCLUDED = {'build', 'out', 'dist', 'target', '.git', '__pycache__'}


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()


def fingerprint(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def source_identity(root):
    """Conservative input closure: all nonignored repository files, including new inputs."""
    root = Path(root)
    def git(*args):
        return subprocess.check_output(['git', *args], cwd=root)
    paths = git('ls-files', '-z', '--cached', '--others', '--exclude-standard').decode().split('\0')
    inputs = {}
    for name in sorted(set(paths) - {''}):
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts or any(part in EXCLUDED for part in relative.parts):
            continue
        path = root / relative
        if path.is_symlink():
            inputs[name] = {'symlink': os.readlink(path)}
        elif path.is_file():
            inputs[name] = {'sha256': digest(path), 'executable': bool(path.stat().st_mode & 0o111)}
        elif not path.exists():
            inputs[name] = {'deleted': True}
        else:
            raise RuntimeError('unsupported source input: ' + name)
    source = dict(commit=git('rev-parse', 'HEAD').decode().strip(), inputs=inputs,
                  scope='Conservative nonignored repository input closure; build/out/dist/target and caches excluded')
    return source | {'identity_sha256': fingerprint(source)}


def validate_source(source):
    if not isinstance(source, dict) or not source.get('commit') or not isinstance(source.get('inputs'), dict):
        raise RuntimeError('candidate manifest has no complete source identity; rebuild candidates')
    if source.get('identity_sha256') != fingerprint({k: v for k, v in source.items() if k != 'identity_sha256'}):
        raise RuntimeError('candidate source identity digest mismatch')


def require_source(expected, actual):
    if expected != actual:
        raise RuntimeError('source changed across candidate build batch; rebuild after source edits settle')


def compiler_identity(compiler, root):
    path = Path(subprocess.check_output(['sh', '-c', 'command -v "$1"', 'sh', compiler], cwd=root, text=True).strip()).resolve()
    return dict(path=str(path), sha256=digest(path),
                version=subprocess.check_output([str(path), '--version'], cwd=root, text=True).strip())


def verify_macros(compiler, root, definitions, observe, header, includes):
    flags = [f'-D{name}={value}' for name, value in definitions.items()]
    command = [compiler, '-ffreestanding', '-E', '-dM', '-x', 'c', *includes, *flags,
               f'-DBOAROS_COST_DIAGNOSTICS={int(observe)}', '-']
    result = subprocess.run(command, cwd=root, input=f'#include "{header}"\n',
                            capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError('effective macro preprocessing failed: ' + shlex.join(command) + '\n' + result.stderr)
    macros = dict(re.findall(r'^#define (\w+) (\d+)$', result.stdout, re.M))
    wanted = definitions | {'BOAROS_COST_DIAGNOSTICS': int(observe)}
    if any(macros.get(name) != str(value) for name, value in wanted.items()):
        raise RuntimeError('effective compiler macros do not match requested candidate')
    return wanted


def build_identity(family, profile, definitions, observe, source, command, compiler, effective):
    validate_source(source)
    return dict(family=family, profile=profile, definitions=definitions,
                observation_enabled=bool(observe), source=source, command=command,
                compiler=compiler, effective_macros=effective)


def cache_matches(work, core, objcopy):
    try:
        saved = json.loads((Path(work) / 'identity.json').read_text())
        return (saved['schema'] == SCHEMA and saved['build_identity'] == core and
                saved['kernel_sha256'] == digest(Path(work) / 'kernel-rv') and
                embedded_identity(Path(work) / 'kernel-rv', objcopy) == {'schema': SCHEMA, 'build_identity': core})
    except (OSError, ValueError, KeyError, RuntimeError):
        return False


def seal_kernel(kernel, core, objcopy):
    """A non-ALLOC section binds provenance to the exact ELF without changing load memory."""
    kernel = Path(kernel)
    with tempfile.TemporaryDirectory(dir=kernel.parent, prefix='identity-') as directory:
        directory = Path(directory)
        note = directory / 'identity.json'
        note.write_bytes(canonical({'schema': SCHEMA, 'build_identity': core}))
        target = directory / 'kernel'
        subprocess.run([objcopy, '--remove-section', SECTION, '--add-section', f'{SECTION}={note}',
                        '--set-section-flags', f'{SECTION}=readonly', str(kernel), str(target)], check=True)
        os.replace(target, kernel)
    return dict(schema=SCHEMA, build_identity=core, binding_sha256=fingerprint(core), kernel_sha256=digest(kernel))


def embedded_identity(kernel, objcopy):
    with tempfile.TemporaryDirectory(prefix='boaros-budget-identity-') as directory:
        target = Path(directory) / 'identity.json'
        # Explicit output keeps objcopy from rewriting the input ELF.
        result = subprocess.run([objcopy, '--dump-section', f'{SECTION}={target}', str(kernel),
                                 str(Path(directory) / 'copy.elf')], capture_output=True, text=True)
        if result.returncode or not target.exists():
            raise RuntimeError('candidate ELF has no bound build manifest; rebuild with this runner')
        try:
            return json.loads(target.read_text())
        except (OSError, ValueError) as error:
            raise RuntimeError('invalid embedded candidate manifest') from error


def validate_core(core, family, profile_parser, definitions_for, observe):
    try:
        if core['family'] != family or type(core['observation_enabled']) is not bool or core['observation_enabled'] != observe:
            raise RuntimeError('candidate manifest family/diagnostic mode mismatch')
        row = profile_parser(core['profile']['name'])
        definitions = definitions_for(row)
        if core['profile'] != row or core['definitions'] != definitions:
            raise RuntimeError('candidate manifest profile/parameters mismatch')
        wanted = definitions | {'BOAROS_COST_DIAGNOSTICS': int(observe)}
        if core['effective_macros'] != wanted:
            raise RuntimeError('candidate manifest effective macro mismatch')
        command = core['command']
        extra = [word.split('=', 1)[1] for word in command if word.startswith('CFLAGS_EXTRA=')]
        mode = [word for word in command if word.startswith('COST_DIAGNOSTICS=')]
        if len(extra) != 1 or sorted(shlex.split(extra[0])) != sorted(f'-D{k}={v}' for k, v in definitions.items()) or mode != [f'COST_DIAGNOSTICS={int(observe)}']:
            raise RuntimeError('candidate manifest compile command/parameters mismatch')
        compiler = core['compiler']
        if not all(compiler.get(key) for key in ('path', 'sha256', 'version')):
            raise RuntimeError('candidate manifest compiler identity missing')
        validate_source(core['source'])
    except (KeyError, TypeError, ValueError) as error:
        raise RuntimeError('invalid candidate build manifest; rebuild with this runner') from error


def validate_label(label, profile_name):
    if CANONICAL.fullmatch(label) and label != profile_name:
        raise RuntimeError('canonical profile label does not match candidate manifest: ' + label)


def load_candidate(label, kernel, family, profile_parser, definitions_for, observe, smoke, objcopy, symbols):
    kernel = Path(kernel)
    observed = bool(re.search(r'\bkernel_(?:socket_protocol_snapshot|cost_begin|cost_control)$', symbols, re.M))
    if observed != observe:
        raise RuntimeError('kernel diagnostics differ from requested mode: ' + label)
    path = kernel.parent / 'identity.json'
    if not path.is_file():
        if not smoke or CANONICAL.fullmatch(label):
            raise RuntimeError('formal/canonical candidate requires a managed build manifest: ' + label)
        return dict(management='unmanaged', kernel_sha256=digest(kernel), source_path=str(kernel),
                    observation_enabled=observe, scope='Functional smoke only; build parameters/source are unknown')
    try:
        manifest = json.loads(path.read_text())
        core = manifest['build_identity']
        validate_core(core, family, profile_parser, definitions_for, observe)
        validate_label(label, core['profile']['name'])
        if manifest['schema'] != SCHEMA or manifest['binding_sha256'] != fingerprint(core):
            raise RuntimeError('candidate manifest binding mismatch')
        if manifest['kernel_sha256'] != digest(kernel):
            raise RuntimeError('candidate kernel hash differs from build manifest')
        if embedded_identity(kernel, objcopy) != {'schema': SCHEMA, 'build_identity': core}:
            raise RuntimeError('candidate manifest differs from embedded ELF identity')
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise RuntimeError('invalid candidate manifest: ' + str(path)) from error
    return dict(management='managed', kernel_sha256=manifest['kernel_sha256'], source_path=str(kernel),
                manifest_sha256=digest(path), manifest=manifest, observation_enabled=observe)


def validate_variants(variants, family, profile_parser, definitions_for, observe, smoke, baseline_sha, baseline_commit, linux_identity):
    common_source, bindings = None, {}
    for label, row in variants.items():
        kind = row.get('management')
        if kind == 'fixed-baseline':
            if label != 'baseline' or observe or row['kernel_sha256'] != baseline_sha or row.get('source_commit') != baseline_commit:
                raise RuntimeError('fixed baseline identity mismatch')
            continue
        if kind == 'fixed-linux':
            if label != 'linux' or observe or not linux_identity or row['kernel_sha256'] != linux_identity.get('image_sha256'):
                raise RuntimeError('unverified Linux reference')
            continue
        if label in ('baseline', 'linux'):
            raise RuntimeError('reserved reference label used by candidate')
        if kind == 'unmanaged' and smoke and not CANONICAL.fullmatch(label):
            continue
        if kind != 'managed':
            raise RuntimeError('candidate requires a managed manifest: ' + label)
        manifest = row['manifest']; core = manifest['build_identity']
        validate_core(core, family, profile_parser, definitions_for, observe)
        validate_label(label, core['profile']['name'])
        if manifest.get('schema') != SCHEMA or manifest.get('binding_sha256') != fingerprint(core) or manifest.get('kernel_sha256') != row['kernel_sha256']:
            raise RuntimeError('candidate manifest/hash binding mismatch')
        source = core['source']
        if common_source is None:
            common_source = source
        elif common_source != source:
            raise RuntimeError('parameter candidates use different build source identities')
        parameters = (core['family'], core['profile']['name'], fingerprint(core['source']), core['observation_enabled'])
        if bindings.setdefault(row['kernel_sha256'], parameters) != parameters:
            raise RuntimeError('same kernel hash claims different candidate parameters')
    return common_source


@contextmanager
def measurement_lock(root, enabled=True):
    if not enabled:
        yield
        return
    path = Path(root) / 'build/cost/measurement.lock'
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError('another network/I/O/cost measurement is active; measurement lock is busy') from error
        try:
            yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)
