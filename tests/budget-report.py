#!/usr/bin/env python3
"""Export compact permanent evidence from complete matched release experiments."""

import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path


def write_table(path, fields, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fields, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def export(works, target):
    if len({work.resolve() for work in works}) != len(works):
        raise ValueError("duplicate experiment directory")
    aggregates = []
    flows = []
    inputs = []
    for work in works:
        identity = json.loads((work / "identity.json").read_text())
        summary = json.loads((work / "summary.json").read_text())
        if (
            summary["status"] != "complete"
            or identity["evidence_kind"] != "performance"
            or identity["repeat"] < 3
        ):
            raise ValueError(
                f"{work}: incomplete/nonrelease evidence cannot become a performance table"
            )
        experiment = work.parent.name + "/" + work.name
        for group in summary["groups"]:
            if group["status"] != "measured":
                raise ValueError("unmeasured group")
            for metric, distribution in group["metrics"].items():
                if not isinstance(distribution.get("median"), (float, int)):
                    continue
                if len(distribution["samples"]) != identity["repeat"]:
                    raise ValueError("partial distribution")
                aggregates.append(
                    dict(
                        experiment=experiment,
                        case=group["case"],
                        variant=group["variant"],
                        boots=identity["repeat"],
                        metric=metric,
                        median=distribution["median"],
                        minimum=distribution["minimum"],
                        maximum=distribution["maximum"],
                    )
                )
        per_flow = defaultdict(list)
        fixtures = {}
        seen = set()
        for path in sorted((work / "runs").glob("*/result.json")):
            row = json.loads(path.read_text())
            case = row["case"]["name"]
            variant = row["variant"]
            key = (case, variant, row["repetition"])
            if row["status"] != "passed" or key in seen:
                raise ValueError("failed/duplicate boot")
            seen.add(key)
            if (
                row["program_sha256"] != identity["program_sha256"]
                or row["kernel_sha256"]
                != identity["variants"][variant]["kernel_sha256"]
            ):
                raise ValueError("input mismatch")
            if (
                fixtures.setdefault(case, row["fixture_sha256"])
                != row["fixture_sha256"]
            ):
                raise ValueError("fixture mismatch")
            network = "connections" in row
            records = row["connections"] if network else row["files"]
            first = min(r["start_ns"] for r in records)
            for r in records:
                role = (
                    "control"
                    if network and row["case"].get("control_id") == r["id"]
                    else "stream"
                    if network
                    else "file"
                )
                per_flow[(case, variant, r["id"], role)].append(
                    dict(
                        bytes=r["receiver_bytes"] if network else r["bytes"],
                        elapsed_ns=r["end_ns"] - r["start_ns"],
                        completion_from_first_ns=r["end_ns"] - first,
                        sync_end_ns=r.get("sync_end_ns", 0),
                        first_ns=first,
                    )
                )
        if len(seen) != summary["expected_boots"]:
            raise ValueError("missing boots")
        for (case, variant, flow_id, role), rows in per_flow.items():
            if len(rows) != identity["repeat"] or len({r["bytes"] for r in rows}) != 1:
                raise ValueError("partial flow")
            result = dict(
                experiment=experiment,
                case=case,
                variant=variant,
                flow_id=flow_id,
                role=role,
                boots=len(rows),
                verified_bytes_per_boot=rows[0]["bytes"],
            )
            for metric in (
                "elapsed_ns",
                "completion_from_first_ns",
                "sync_from_first_ns",
            ):
                samples = (
                    [
                        r["sync_end_ns"] - r["first_ns"] if r["sync_end_ns"] else 0
                        for r in rows
                    ]
                    if metric == "sync_from_first_ns"
                    else [r[metric] for r in rows]
                )
                result.update(
                    {
                        metric + "_median": statistics.median(samples),
                        metric + "_minimum": min(samples),
                        metric + "_maximum": max(samples),
                    }
                )
            flows.append(result)
        for case, fixture in fixtures.items():
            for variant, meta in identity["variants"].items():
                build = meta.get("manifest", {}).get("build_identity", {})
                source = build.get("source", {}).get(
                    "commit",
                    identity["baseline_commit"]
                    if variant == "baseline"
                    else identity["linux_identity"]["inputs"]["source"][1]
                    if variant == "linux"
                    else "",
                )
                inputs.append(
                    dict(
                        experiment=experiment,
                        case=case,
                        variant=variant,
                        program_sha256=identity["program_sha256"],
                        kernel_sha256=meta["kernel_sha256"],
                        kernel_source=source,
                        runtime_source=identity["runtime_source"]["commit"],
                        fixture_sha256=fixture,
                        qemu_sha256=identity["qemu_sha256"],
                        ram=identity["ram"],
                        transport=identity["transport"],
                    )
                )
    target.mkdir(parents=True, exist_ok=True)
    for name, rows in [("summary", aggregates), ("flows", flows), ("inputs", inputs)]:
        if not rows:
            raise ValueError("empty table")
        write_table(target / f"data-path-budget-{name}.tsv", list(rows[0]), rows)
        print(name, len(rows))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("work", nargs="+", type=Path)
    args = parser.parse_args()
    export(args.work, args.output)
