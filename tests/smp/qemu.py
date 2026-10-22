#!/usr/bin/env python3
"""Run the existing userland consumer unchanged in an explicit MTTCG profile."""
import json
import math
import os
from pathlib import Path
import selectors
import subprocess
import sys
import time
from riscv import identity,observe_threads


def execution_timeout():
    budget = float(os.environ.get('BOAROS_SMP_TIMEOUT', '180'))
    if not math.isfinite(budget) or not 0 < budget <= 180:
        raise ValueError('QEMU execution budget must be in (0,180] seconds')
    return budget


def main():
    args = sys.argv[1:]
    actual = os.environ["BOAROS_SMP_QEMU"]
    firmware = os.environ.get("BOAROS_SMP_FIRMWARE", "default")
    output = Path(os.environ["BOAROS_SMP_OUTPUT"])
    # 原consumer保留盘准备、输入、完整marker与owner判定；仅改变架构profile。
    cpus = int(os.environ.get("BOAROS_SMP_CPUS", "2"))
    budget = execution_timeout()
    for flag, value in (("-smp", str(cpus)), ("-bios", firmware)):
        index = args.index(flag)
        args[index + 1] = value
    args += ["-accel", "tcg,thread=multi", "-name", "boaros-smp,debug-threads=on"]
    kernel = args[args.index("-kernel") + 1]
    image = next(part.split("=", 1)[1] for value in args for part in value.split(",")
                 if part.startswith("file="))
    output.mkdir(parents=True, exist_ok=True)
    prefix = output / Path(image).stem
    if os.environ.get("BOAROS_SMP_DISK") == "1":
        disk=output/(Path(image).stem+"-controlled.img")
        subprocess.run(["truncate","-s","32M",str(disk)],check=True)
        subprocess.run(["mkfs.ext4","-q","-F",str(disk)],check=True)
        args += ["-global","virtio-mmio.force-legacy=false","-drive",f"file={disk},if=none,format=raw,cache=writeback,id=controlled",
                 "-device","virtio-blk-device,drive=controlled,bus=virtio-mmio-bus.1"]
    else: disk=None
    record = {"command": [actual, *args], "kernel": identity(kernel),
              "qemu": identity(actual), "disk_before": identity(image),
              "firmware": identity(firmware) if firmware != "default" else {"mode": "default"}}
    if disk: record["controlled_disk_before"]=identity(disk)
    start = time.monotonic()
    process = subprocess.Popen(record["command"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    os.set_blocking(process.stdout.fileno(), False)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    threads, data, timed_out = set(), bytearray(), False
    try:
        while process.poll() is None:
            threads.update(observe_threads(process.pid))
            for key, _ in selector.select(0.005):
                chunk=os.read(key.fd,65536)
                data.extend(chunk)
                sys.stdout.buffer.write(chunk);sys.stdout.buffer.flush()
            if time.monotonic() - start >= budget:
                timed_out = True
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                break
        process.wait(timeout=2)
        os.set_blocking(process.stdout.fileno(), True)
        tail=process.stdout.read();data.extend(tail)
        sys.stdout.buffer.write(tail);sys.stdout.buffer.flush()
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        selector.close()
        process.stdout.close()
    text = data.decode(errors="replace")
    prefix.with_suffix(".log").write_text(text)
    record.update(returncode=process.returncode, timeout=timed_out, budget_seconds=budget, elapsed_seconds=time.monotonic() - start,
                  vcpu_threads=sorted(t for t in threads if "/TCG" in t))
    prefix.with_suffix(".json").write_text(json.dumps(record, indent=2) + "\n")
    if timed_out or process.returncode or len(record["vcpu_threads"]) != cpus or "SMP failure" in text or "SMP fatal" in text:
        return 1
    if os.environ.get("BOAROS_USER_SMP") == "1":
        # 原consumer仍负责完整行为/PID1回收；本层保存实际MTTCG执行身份。
        return 0
    if "SMP root overlap counter=" not in text or "controller/arena/wait owners returned" not in text:
        return 1
    if os.environ.get("BOAROS_SMP_FILES")=="1":
        if ("FILES SMP native FD reuse/early wake/EOF/cancel/content/task rollback passed" not in text or
            "FILES SMP measured live-user overlap packets=" not in text or
            "FD/OFD/pipe/wait owners returned" not in text): return 1
    if os.environ.get("BOAROS_SMP_FILE_IO")=="1":
        if ("FILE IO native cold buffer/offset/prefix/sync/close owners passed" not in text or
            "FILE IO live user overlap transfers=" not in text): return 1
    if disk:
        if "disk MM CPU0 I/O/cache/alias/mount owners returned" not in text: return 1
        subprocess.run(["e2fsck","-fn",str(disk)],check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
