#!/usr/bin/env python3
"""Rebuild the documented data-path workload subsets without hidden case selection."""

import argparse
import itertools


def cases(name):
    if name == "network-screen":
        return ["loopback:nonblocking:mixed:5:rx", "tap:nonblocking:mixed:5:tx"]
    if name == "network-near":
        return ["tap:nonblocking:mixed:near:tx"]
    if name == "network-observe":
        return cases("network-screen") + cases("network-near")
    if name == "network-receive":
        return ["tap:blocking:mixed:near:rx"]
    if name == "network-expanded":
        result = []
        for path, mode in itertools.product(
            ("loopback", "tap"), ("blocking", "nonblocking")
        ):
            for count in (1, 5):
                result.append(f"{path}:{mode}:rr:{count}:rx")
                for direction in ("rx", "tx"):
                    result.append(f"{path}:{mode}:bulk:{count}:{direction}")
            direction = "rx" if mode == "blocking" else "tx"
            result.append(f"{path}:{mode}:mixed:near:{direction}")
        return result + ["loopback:blocking:rr:near:rx", "tap:nonblocking:rr:near:rx"]
    if name == "io-screen":
        return ["ext4:append:fsync:1:16M:64K", "ext4:cold-read:cache:4:4M:4K"]
    if name == "io-copy-growth":
        return ["ext4:append:cache:1:64M:1K", "ext4:overwrite:fsync:1:16M:64K"]
    if name == "io-fdatasync":
        return ["ext4:overwrite:fdatasync:4:4M:4K"]
    if name == "io-expanded":
        result = [
            f"ext4:append:cache:1:{size}M:{request}K"
            for size, request in itertools.product((1, 4, 16, 64), (1, 4, 64))
        ]
        result += [
            f"ext4:append:fsync:1:{size}M:{request}K"
            for size, request in ((1, 4), (4, 4), (16, 64), (64, 4))
        ]
        return result + [
            "ext4:overwrite:cache:1:16M:64K",
            "ext4:overwrite:fsync:1:16M:64K",
            "ext4:cold-read:cache:1:16M:4K",
            "ext4:hot-read:cache:1:16M:4K",
            "ext4:cold-read:cache:4:4M:4K",
            "ext4:append:fsync:4:4M:4K",
            "tmpfs:append:cache:1:16M:1K",
            "tmpfs:read:cache:1:16M:64K",
            "tmpfs:overwrite:cache:1:16M:4K",
        ]
    raise ValueError(name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "name",
        choices=(
            "network-screen",
            "network-near",
            "network-expanded",
            "network-observe",
            "network-receive",
            "io-screen",
            "io-expanded",
            "io-copy-growth",
            "io-fdatasync",
        ),
    )
    args = parser.parse_args()
    print(",".join(cases(args.name)))
