#!/usr/bin/env python3
"""Embed pahole's detached kernel BTF in an allocated, readonly ELF section."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('btf', type=Path)
    parser.add_argument('assembly', type=Path)
    args = parser.parse_args()
    args.assembly.write_text(
        '.section .rodata.boaros_btf,"a",@progbits\n'
        '.balign 4\n'
        '.global __boaros_btf_start\n'
        '.global __boaros_btf_end\n'
        '.type __boaros_btf_start,@object\n'
        '__boaros_btf_start:\n'
        '.incbin ' + json.dumps(str(args.btf.resolve(strict=True))) + '\n'
        '__boaros_btf_end:\n'
        '.size __boaros_btf_start,.-__boaros_btf_start\n'
        '.section .note.GNU-stack,"",@progbits\n')


if __name__ == '__main__':
    main()
