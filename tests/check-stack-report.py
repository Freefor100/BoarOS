#!/usr/bin/env python3
"""Require measured reserve from completed production task stacks."""
import re
import sys
from pathlib import Path

if len(sys.argv) < 2:
    raise SystemExit("usage: check-stack-report.py LOG [LOG ...]")

for name in sys.argv[1:]:
    reports = re.findall(
        r"BoarOS: task stacks released=(0x[0-9a-f]+) "
        r"min-free=(0x[0-9a-f]+) max-used=(0x[0-9a-f]+)",
        Path(name).read_text(),
    )
    if len(reports) != 1:
        raise SystemExit(f"{name}: expected one completed-task stack report")
    released, free, used = (int(value, 16) for value in reports[0])
    if released == 0 or used == 0 or free < 1024:
        raise SystemExit(f"{name}: stack reserve failed: {released=} {free=} {used=}")
    print(f"task stacks: released={released}, minimum reserve={free}, peak use={used} bytes")
