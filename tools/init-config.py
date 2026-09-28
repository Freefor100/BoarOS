#!/usr/bin/env python3
"""Validate an immutable initial-process configuration and emit a C header."""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]

def constant(path, name):
    return int(re.search(r'#define\s+' + name + r'\s+(\d+)U', (ROOT / path).read_text())[1])

def render(config):
    if not isinstance(config, dict) or set(config) != {'path', 'argv', 'envp'}:
        raise ValueError('expected exactly path, argv and envp')
    def raw(value):
        if not isinstance(value, str) or '\0' in value:
            raise ValueError('strings must be text without NUL')
        return value.encode('utf-8')
    path = raw(config['path'])
    if not path.startswith(b'/') or len(path) >= constant('include/kernel/fs_context.h', 'KERNEL_FS_PATH_MAX'):
        raise ValueError('initial ELF path must be absolute and fit the path limit')
    vectors = []
    limit = constant('include/kernel/exec_image.h', 'KERNEL_EXEC_STRING_LIMIT')
    for key in ('argv', 'envp'):
        value = config[key]
        if not isinstance(value, list) or len(value) > limit // 8:
            raise ValueError('argument/environment vector exceeds exec limit')
        vectors.append([raw(item) for item in value])
    if not vectors[0]:
        raise ValueError('argv must contain argv[0]')
    if sum(len(x) + 1 for vector in vectors for x in vector) > limit:
        raise ValueError('strings exceed exec limit')
    def literal(data):
        return '"' + ''.join(f'\\{byte:03o}' for byte in data) + '"'
    lines = ['/* Generated from INIT_CONFIG; do not edit. */', '#include <kernel/exec_image.h>', f'static const char init_path[] = {literal(path)};']
    for name, vector in zip(('init_arguments', 'init_environment'), vectors):
        lines.append(f'static const struct kernel_exec_string {name}[] = {{')
        lines.extend(f'    {{{literal(item)}, {len(item)}U}},' for item in vector)
        if not vector:
            lines.append('    {0, 0},')
        lines += ['};', f'#define {name.upper()}_COUNT {len(vector)}U']
    return '\n'.join(lines) + '\n'

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = render(json.loads(args.input.read_text()))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != result:
        args.output.write_text(result)

if __name__ == '__main__':
    main()
