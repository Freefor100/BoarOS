#!/usr/bin/env python3
"""Exec the fixed QEMU with an explicit diagnostic receipt; never run a second boot."""
import json
import os
from pathlib import Path
import sys

from cost_diagnostic import (PROFILE, check_manifest, execution_config, wrapper_arguments)
from cost_match import canonical, digest


def main():
    config_path = Path(os.environ['BOAROS_DIAG_CONFIG'])
    config = json.loads(config_path.read_text())
    manifest = json.loads(Path(config['manifest_path']).read_text())
    check_manifest(manifest)
    output = Path(config['manifest_path']).parent
    if execution_config(manifest, config['entry'], output) != config:
        raise ValueError('wrapper configuration differs from sealed manifest')
    spent = output / 'attempts' / f"{config['entry']['ordinal']:02d}.spent"
    if json.loads(spent.read_text()) != config['entry']:
        raise ValueError('attempt not reserved')
    for name in ('qemu', 'observer'):
        item = manifest['inputs'][name]
        if digest(Path(item['path'])) != item['sha256']:
            raise ValueError('execution input changed: ' + name)
    argv, boot = wrapper_arguments(config, sys.argv[1:])
    if boot:
        # Exclusive receipt is the final gate before exec; even exec failure consumes this start.
        with Path(config['receipt_path']).open('x') as stream:
            stream.write(canonical(dict(config['expected'], profile=PROFILE, acceptance=False)))
            stream.flush()
            os.fsync(stream.fileno())
    os.execv(argv[0], argv)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print('diagnostic QEMU refused: ' + str(error), file=sys.stderr)
        sys.exit(2)
