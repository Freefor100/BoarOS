#!/usr/bin/env python3
"""Keep diagnostic scopes/replica counts explicit; never call these release rates."""

import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path

COST = (
    "resize_visits",
    "resize_tail_pages",
    "resize_alias_rearms",
    "cache_probes",
    "writeback_visits",
    "snapshot_copy",
    "writeback_requested",
    "writeback_accepted",
    "block_cache_hits",
    "block_cache_misses",
    "stream_calls",
    "stream_accepted",
    "stream_copy",
    "stream_resolutions",
    "stream_admit_blocked",
    "stream_protocol_blocked",
    "stream_copy_blocked_bytes",
    "network_service_calls",
    "network_socket_visits",
    "network_global_scans",
    "network_loop_packets",
    "network_timer_callbacks",
    "network_runnable_sleep",
    "network_poll_calls",
    "network_poll_services",
    "wakes",
    "switches",
)
LATENCY = (
    "tx-clock-hz",
    "tx-done-free-count",
    "tx-done-free-ticks",
    "tx-done-free-max",
    "tx-free-post-count",
    "tx-free-post-ticks",
    "tx-free-post-max",
    "tx-latency-overflow",
)


def export(works, target):
    if len({work.resolve() for work in works}) != len(works):
        raise ValueError("duplicate experiment directory")
    values = defaultdict(list)
    identities = {}
    boot_counts = defaultdict(int)
    input_details = {}

    def add(group, metric, unit, value, scope):
        if value is None:
            raise ValueError("unknown diagnostic field " + metric)
        values[group + (metric, unit, scope)].append(value)

    for work in works:
        identity = json.loads((work / "identity.json").read_text())
        summary = json.loads((work / "summary.json").read_text())
        if identity["evidence_kind"] != "diagnostic" or summary["status"] != "complete":
            raise ValueError("incomplete/non-diagnostic input " + str(work))
        rows = [
            json.loads(p.read_text())
            for p in sorted((work / "runs").glob("*/result.json"))
        ]
        if len(rows) != summary["expected_boots"]:
            raise ValueError("missing diagnostic boot")
        seen = set()
        for row in rows:
            key = (row["case"]["name"], row["variant"], row["repetition"])
            if key in seen:
                raise ValueError("duplicate diagnostic boot")
            seen.add(key)
            if (
                row["status"] != "passed"
                or row["cost"]["overflow"]
                or row["cost"]["state"] != "complete"
            ):
                raise ValueError("invalid diagnostic result")
            family = "network" if "protocol_snapshots" in row else "io"
            group = (family, row["case"]["name"], row["variant"])
            identities_now = (
                row["kernel_sha256"],
                row["program_sha256"],
                identity["qemu_sha256"],
                identity["ram"],
                identity["transport"],
            )
            if identities.setdefault(group, identities_now) != identities_now:
                raise ValueError("mixed diagnostic input identity")
            boot_counts[group] += 1
            meta = identity["variants"][row["variant"]]
            arrays = meta.get("static_storage_arrays", {})
            input_details[group] = dict(
                kernel_source=meta["manifest"]["build_identity"]["source"]["commit"],
                runtime_source=identity["runtime_source"]["commit"],
                static_protocol_arrays_bytes=sum(arrays.values()) if arrays else "NA",
            )
            amount = (
                row["metrics"]["receiver_bytes"]
                if family == "network"
                else row["metrics"]["bytes"]
            )
            mib = amount / 1048576
            scope = "Active COST lanes, including setup/teardown inside the epoch; excludes observer lane."
            norm_scope = scope + (
                " Per verified bulk MiB; mixed RR traffic is included in costs. Loopback counts both guest endpoints, TAP only guest."
                if family == "network"
                else " Per verified I/O MiB."
            )
            for metric in COST:
                number = sum(
                    row["cost"][lane + "." + metric + ".value"]
                    for lane in ("foreground", "background")
                )
                unit = row["cost"]["foreground." + metric + ".unit"]
                add(group, "cost." + metric, unit, number, scope)
                add(
                    group,
                    "cost_per_mib." + metric,
                    unit + "/MiB",
                    number / mib,
                    norm_scope,
                )
            memory = row.get("managed_memory_peaks", row.get("memory_peaks"))
            for metric, number in memory.items():
                add(
                    group,
                    "memory." + metric,
                    "bytes",
                    number,
                    "Allocator/owning OFD heap lifetime to snapshot, before COST text formatting; heap is a subset of managed pages; reserved image/static protocol arrays excluded.",
                )
            add(
                group,
                "diagnostic.receiver_mbit_s"
                if family == "network"
                else "diagnostic.completion_mib_s",
                "Mbit/s" if family == "network" else "MiB/s",
                row["metrics"][
                    "receiver_mbit_s" if family == "network" else "completion_mib_s"
                ],
                "Observed build, not a release throughput result.",
            )
            if family == "network":
                before = row["protocol_snapshots"]["before"]
                after = row["protocol_snapshots"]["after"]
                if before["counter_bits"] != 32 or after["counter_bits"] != 32:
                    raise ValueError("unexpected protocol counter width")
                for metric in (
                    "heap_max",
                    "tcp_segment_peak",
                    "pbuf_peak",
                    "pbuf_pool_peak",
                    "tcp_active_peak",
                    "tcp_listen_peak",
                ):
                    add(
                        group,
                        "protocol." + metric,
                        "bytes" if metric == "heap_max" else "count",
                        after[metric],
                        "lwIP lifetime high watermark; static backing budget is reported separately.",
                    )
                for metric in (
                    "tcp_write_calls",
                    "tcp_written_bytes",
                    "tcp_xmit",
                    "tcp_recv",
                    "tcp_memerr",
                ):
                    if after[metric] < before[metric]:
                        raise ValueError(
                            "protocol counter wrap needs explicit treatment"
                        )
                    delta = after[metric] - before[metric]
                    add(
                        group,
                        "protocol_delta." + metric,
                        "bytes" if metric == "tcp_written_bytes" else "count",
                        delta,
                        "Between protocol snapshots around COST epoch; TCP transmit attempts are not wire packets.",
                    )
                    add(
                        group,
                        "protocol_per_mib." + metric,
                        ("bytes" if metric == "tcp_written_bytes" else "count")
                        + "/MiB",
                        delta / mib,
                        "Per verified bulk MiB, including mixed RR/setup/teardown protocol work.",
                    )
                if row["case"]["path"] == "tap":
                    driver = row["driver_statistics"]
                    if (
                        not set(LATENCY) <= driver.keys()
                        or not driver["tx-clock-hz"]
                        or driver["tx-latency-overflow"]
                    ):
                        raise ValueError("missing/overflowed driver latency statistics")
                    for metric in LATENCY:
                        add(
                            group,
                            "driver." + metric,
                            "Hz"
                            if metric == "tx-clock-hz"
                            else "ticks"
                            if "ticks" in metric or "max" in metric
                            else "count",
                            driver[metric],
                            "Device lifetime including setup and shutdown; DONE is software harvest, FREE-to-post includes idle reuse.",
                        )
                    for prefix in ("tx-done-free", "tx-free-post"):
                        if not driver[prefix + "-count"]:
                            raise ValueError("no TX latency observations")
                        add(
                            group,
                            "driver." + prefix + "-mean-us",
                            "us",
                            driver[prefix + "-ticks"]
                            / driver[prefix + "-count"]
                            / driver["tx-clock-hz"]
                            * 1e6,
                            "Per-boot arithmetic mean; software slot lifetime, not hardware completion or ready-only wait.",
                        )
                        add(
                            group,
                            "driver." + prefix + "-max-us",
                            "us",
                            driver[prefix + "-max"] / driver["tx-clock-hz"] * 1e6,
                            "Per-boot maximum; FREE-to-post includes legitimate idle slots.",
                        )
                    for metric in ("tx", "rx", "tx-sg", "tx-copy", "drops", "errors"):
                        add(
                            group,
                            "driver." + metric,
                            "count",
                            driver[metric],
                            "Device lifetime including setup and shutdown.",
                        )
    result = []
    scopes = {
        scope: f"S{i + 1}"
        for i, scope in enumerate(sorted({key[-1] for key in values}))
    }
    for key, samples in sorted(values.items()):
        family, case, variant, metric, unit, scope = key
        if len(samples) != boot_counts[key[:3]]:
            raise ValueError("partial metric")
        kernel, program, qemu, ram, transport = identities[key[:3]]
        result.append(
            dict(
                family=family,
                case=case,
                variant=variant,
                boots=len(samples),
                metric=metric,
                unit=unit,
                median=statistics.median(samples),
                minimum=min(samples),
                maximum=max(samples),
                scope=scopes[scope],
            )
        )
    if not result:
        raise ValueError("no diagnostics")
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("w", newline="") as stream:
        writer = csv.DictWriter(
            stream, list(result[0]), delimiter="\t", lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(result)
    input_rows = []
    for group in sorted(identities):
        kernel, program, qemu, ram, transport = identities[group]
        input_rows.append(
            dict(
                family=group[0],
                case=group[1],
                variant=group[2],
                boots=boot_counts[group],
                kernel_sha256=kernel,
                program_sha256=program,
                qemu_sha256=qemu,
                ram=ram,
                transport=transport,
                **input_details[group],
            )
        )
    scope_rows = [
        dict(scope=identifier, description=scope)
        for scope, identifier in scopes.items()
    ]
    for suffix, rows in [("inputs", input_rows), ("scopes", scope_rows)]:
        with target.with_name(target.stem + "-" + suffix + target.suffix).open(
            "w", newline=""
        ) as stream:
            writer = csv.DictWriter(
                stream, list(rows[0]), delimiter="\t", lineterminator="\n"
            )
            writer.writeheader()
            writer.writerows(rows)
    print("diagnostic rows", len(result), "boots", sum(boot_counts.values()))


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("work", nargs="+", type=Path)
    a = p.parse_args()
    export(a.work, a.output)
