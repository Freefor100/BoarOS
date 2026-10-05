"""Native ELF fixtures; these tests do not build or boot a BoarOS kernel."""
import copy
import json
from pathlib import Path
import subprocess

import budget_identity as contracts


def source(tag='A'):
    value = dict(commit=tag, inputs={'unit.c': {'sha256': contracts.fingerprint(tag), 'executable': False}}, scope='test input')
    return value | {'identity_sha256': contracts.fingerprint(value)}


def candidate(directory, module, family, name, observe=False, source_tag='A'):
    directory = Path(directory); directory.mkdir(parents=True)
    kernel = directory / 'kernel-rv'
    code = 'int main(void) { return 0; }\n'
    if observe:
        code += 'void kernel_cost_begin(void) {}\n'
    subprocess.run(['cc', '-x', 'c', '-', '-o', str(kernel)], input=code, text=True, check=True)
    profile = module.profile(name); definitions = module.definitions(profile)
    command = ['make', 'all', f'COST_DIAGNOSTICS={int(observe)}',
               'CFLAGS_EXTRA=' + ' '.join(f'-D{k}={v}' for k, v in definitions.items())]
    core = contracts.build_identity(family, profile, definitions, observe, source(source_tag), command,
        contracts.compiler_identity('cc', directory), definitions | {'BOAROS_COST_DIAGNOSTICS': int(observe)})
    manifest = contracts.seal_kernel(kernel, core, 'objcopy')
    save_manifest(kernel, manifest)
    return kernel, manifest


def save_manifest(kernel, manifest):
    (Path(kernel).parent / 'identity.json').write_text(json.dumps(manifest))


def load(module, family, kernel, label='current', observe=False, smoke=False):
    symbols = subprocess.check_output(['nm', str(kernel)], text=True)
    return contracts.load_candidate(label, kernel, family, module.profile, module.definitions, observe, smoke, 'objcopy', symbols)


def changed_manifest(manifest, **changes):
    result = copy.deepcopy(manifest)
    result['build_identity'].update(changes)
    result['binding_sha256'] = contracts.fingerprint(result['build_identity'])
    return result
