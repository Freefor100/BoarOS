#!/usr/bin/env python3
"""Plot verified complete release summaries; no diagnostic timings are accepted."""

import argparse
import json
import csv
import math
from pathlib import Path
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, ScalarFormatter


def groups(work):
    data = json.loads((work / "summary.json").read_text())
    if (
        data["status"] != "complete"
        or data["evidence_kind"] != "performance"
        or any(g["status"] != "measured" for g in data["groups"])
    ):
        raise ValueError("plots require complete matched release distributions")
    return {(g["case"], g["variant"]): g["metrics"] for g in data["groups"]}


def table_groups(path, experiment):
    result = {}
    for row in csv.DictReader(path.open(), delimiter="\t"):
        if row["experiment"] != experiment:
            continue
        if int(row["boots"]) < 3:
            raise ValueError("incomplete distribution")
        values = {k: float(row[k]) for k in ("median", "minimum", "maximum")}
        if (
            not all(math.isfinite(v) for v in values.values())
            or not values["minimum"] <= values["median"] <= values["maximum"]
        ):
            raise ValueError("invalid distribution")
        group = result.setdefault((row["case"], row["variant"]), {})
        if row["metric"] in group:
            raise ValueError("duplicate metric")
        group[row["metric"]] = values
    if not result:
        raise ValueError("missing experiment " + experiment)
    return result


def plot(io, network, target, table=None):
    target.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update(
        {
            "font.family": "DejaVu Sans",
            "font.size": 10,
            "axes.spines.top": False,
            "axes.spines.right": False,
        }
    )
    data = table_groups(table, "io-budget/formal-expanded") if table else groups(io)
    fig, axes = plt.subplots(1, 3, figsize=(12, 4.3))
    profiles = [
        ("baseline", "5687377 baseline", "#777777"),
        ("ra0-wb1", "Current default (RA0 / WB1)", "#0072b2"),
        ("ra0-wb8", "RA0 / WB8", "#009e73"),
        ("ra8-wb8", "RA8 / WB8", "#cc79a7"),
    ]
    for axis, request in zip(axes, (1, 4, 64)):
        for profile, label, color in profiles:
            values = [
                data[(f"ext4:append:cache:1:{size}M:{request}K", profile)][
                    "completion_mib_s"
                ]
                for size in (1, 4, 16, 64)
            ]
            middle = [v["median"] for v in values]
            axis.errorbar(
                (1, 4, 16, 64),
                middle,
                yerr=(
                    [v["median"] - v["minimum"] for v in values],
                    [v["maximum"] - v["median"] for v in values],
                ),
                label=label,
                color=color,
                marker="o",
                capsize=3,
                linewidth=1.5,
            )
        axis.set_xscale("log", base=4)
        axis.set_xticks((1, 4, 16, 64), ("1", "4", "16", "64"))
        axis.set_title(f"{request} KiB write requests")
        axis.set_xlabel("File size (MiB)")
        axis.set_ylabel("Cache completion (MiB/s)")
        axis.set_ylim(bottom=0)
        axis.grid(alpha=0.18)
    fig.suptitle(
        "Append scaling after removing repeated cache growth scans", fontsize=14, y=0.98
    )
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(
        handles,
        labels,
        loc="lower center",
        ncol=4,
        bbox_to_anchor=(0.5, 0.04),
        frameon=False,
    )
    fig.text(
        0.5,
        0.014,
        "Median and full range of 3 independent boots. Includes pattern generation; fsync/readback are outside this window.",
        ha="center",
        fontsize=9,
    )
    fig.tight_layout(rect=(0, 0.13, 1, 0.93))
    for ext in ("png", "svg"):
        fig.savefig(target / f"data-path-append-scaling.{ext}", dpi=160)
    plt.close(fig)
    data = (
        table_groups(table, "network-budget/formal-near-v2")
        if table
        else groups(network)
    )
    case = "tap:nonblocking:mixed:near:tx"
    fig, axis = plt.subplots(figsize=(9, 5.6))
    styles = {8: ("#0072b2", "o"), 16: ("#d55e00", "^"), 32: ("#cc79a7", "s")}
    for window, (color, marker) in styles.items():
        for pool in (1, 2, 4):
            for heap in (1, 2, 4):
                name = f"w{window}-p{pool}-m{heap}"
                v = data[(case, name)]
                x = v["receiver_mbit_s"]
                y = v["control_rr_during_bulk_p99_ns"]
                axis.errorbar(
                    x["median"],
                    y["median"] / 1e6,
                    xerr=[[x["median"] - x["minimum"]], [x["maximum"] - x["median"]]],
                    yerr=[
                        [(y["median"] - y["minimum"]) / 1e6],
                        [(y["maximum"] - y["median"]) / 1e6],
                    ],
                    fmt=marker,
                    color=color,
                    alpha=0.8,
                    markersize=5,
                    capsize=2,
                    label=f"{window} MSS" if pool == heap == 1 else None,
                )
    v = data[(case, "baseline")]
    x = v["receiver_mbit_s"]
    y = v["control_rr_during_bulk_p99_ns"]
    axis.errorbar(
        x["median"],
        y["median"] / 1e6,
        xerr=[[x["median"] - x["minimum"]], [x["maximum"] - x["median"]]],
        yerr=[
            [(y["median"] - y["minimum"]) / 1e6],
            [(y["maximum"] - y["median"]) / 1e6],
        ],
        color="#333333",
        fmt="*",
        markersize=12,
        capsize=2,
        label="5687377 baseline",
        zorder=4,
    )
    for name, label, offset in [
        ("baseline", "Baseline", (-85, -20)),
        ("w8-p1-m1", "Default: 8 / 1 / 1", (-75, -35)),
        ("w8-p4-m2", "8 / 4 / 2", (-75, 22)),
        ("w16-p2-m2", "16 / 2 / 2", (35, 4)),
    ]:
        v = data[(case, name)]
        axis.annotate(
            label,
            (
                v["receiver_mbit_s"]["median"],
                v["control_rr_during_bulk_p99_ns"]["median"] / 1e6,
            ),
            xytext=offset,
            textcoords="offset points",
            arrowprops={"arrowstyle": "-", "color": "#888888"},
            fontsize=10,
        )
    axis.set_yscale("log")
    axis.yaxis.set_major_locator(FixedLocator([10, 20, 50, 100, 200, 500]))
    axis.yaxis.set_major_formatter(ScalarFormatter())
    axis.set_xlabel("Verified bulk receive rate (Mbit/s), higher is better")
    axis.set_ylabel("Control RR P99 (ms, log scale), lower is better")
    axis.grid(alpha=0.2)
    axis.set_title("TCP budget tradeoffs near the fixed PCB limit", fontsize=14, pad=14)
    axis.legend(loc="upper center", ncol=4, frameon=False)
    fig.text(
        0.5,
        0.012,
        "27 bulk + 1 control stream; 4 MiB per bulk stream. 3 boots per candidate; error bars show full ranges.\nLabels are MSS / pool multiplier / heap multiplier. RR requests begun during bulk include late replies.",
        ha="center",
        fontsize=9,
    )
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    for ext in ("png", "svg"):
        fig.savefig(target / f"data-path-tcp-budgets.{ext}", dpi=160)
    plt.close(fig)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--table", type=Path)
    p.add_argument("--io", type=Path)
    p.add_argument("--network", type=Path)
    p.add_argument("--output", required=True, type=Path)
    a = p.parse_args()
    if not a.table and (not a.io or not a.network):
        p.error("provide --table or both --io and --network")
    plot(a.io, a.network, a.output, a.table)
