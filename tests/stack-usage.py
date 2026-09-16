#!/usr/bin/env python3
"""Check compiler frame bounds; this does not prove whole-call-chain bounds.

The assembly Trap Frame and the measured-reserve requirement are deducted
explicitly. High-water workload checks remain necessary for accumulated
frames, recursion and indirect calls.
"""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path)
parser.add_argument("--stack-bytes", type=int, required=True)
parser.add_argument("--trap-frame-bytes", type=int, required=True)
parser.add_argument("--reserve-bytes", type=int, default=1024)
parser.add_argument("--guard-bytes", type=int, default=16)
args = parser.parse_args()
budget = args.stack_bytes - args.guard_bytes - args.trap_frame_bytes - args.reserve_bytes
reports = sorted(args.build.rglob("*.su"))
if not reports or budget <= 0:
    raise SystemExit("missing stack-usage reports or invalid stack budget")
failures = []
source_root = Path(__file__).resolve().parent.parent
for obj in args.build.rglob("*.o"):
    source = source_root / obj.relative_to(args.build).with_suffix(".c")
    if source.is_file() and not obj.with_suffix(".su").is_file():
        failures.append(f"{obj}: missing compiler stack-usage report")
count = 0
largest = (0, "")
for report in reports:
    for line in report.read_text().splitlines():
        try:
            function, size, kind = line.split("\t")
            size = int(size)
        except ValueError:
            failures.append(f"{report}: invalid stack-usage record: {line}")
            continue
        count += 1
        largest = max(largest, (size, function))
        if kind not in {"static", "dynamic,bounded"}:
            failures.append(f"{function}: uncontrolled stack usage ({kind})")
        if size > budget:
            failures.append(f"{function}: frame {size} exceeds {budget}-byte budget")
if count == 0:
    failures.append("empty compiler stack-usage reports")
if failures:
    raise SystemExit("\n".join(failures))
print(f"compiler stack bounds: {count} functions; largest={largest[0]} bytes "
      f"({largest[1]}); assembly trap={args.trap_frame_bytes}, reserve={args.reserve_bytes}")
