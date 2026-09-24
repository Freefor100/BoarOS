#!/usr/bin/env python3
"""Prune reproducible images from completed passing inventory cases."""

import argparse
import json
from pathlib import Path

import suites

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', nargs='?', type=Path, default=ROOT / 'build')
    parser.add_argument('--apply', action='store_true',
                        help='delete eligible images after checking all evidence')
    args = parser.parse_args()
    candidates = []
    for suite in sorted(args.root.resolve().rglob('suite.json')):
        state = json.loads(suite.read_text())
        if state.get('status') not in ('complete', 'partial'):
            continue
        for case_id, row in state['results'].items():
            if row.get('status') == 'pass' and row.get('completed'):
                directory = suite.parent / 'cases' / case_id
                candidates.append((directory, row))
    eligible = [(directory, row, size) for directory, row in candidates
                if (size := suites.prune_pass_images(directory, row, dry_run=True))]
    reclaimable = sum(size for _, _, size in eligible)
    if args.apply:
        for directory, row, _ in eligible:
            suites.prune_pass_images(directory, row)
    action = 'Removed' if args.apply else 'Eligible'
    print(f'{action}: {len(eligible)} cases, '
          f'{reclaimable / 1024 ** 3:.2f} GiB allocated images '
          f'({len(candidates)} passing records checked)')


if __name__ == '__main__':
    main()
