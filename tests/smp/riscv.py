#!/usr/bin/env python3
"""Real MTTCG execution, complete exit and resource evidence, per boot."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import time


def identity(path):
    resolved = Path(shutil.which(str(path)) or path).resolve(strict=True)
    return {"path": str(resolved), "sha256": hashlib.sha256(resolved.read_bytes()).hexdigest()}


def boot(qemu, firmware, kernel, memory, cpus, output):
    command = [qemu, "-machine", "virt", "-accel", "tcg,thread=multi", "-smp", str(cpus),
               "-m", memory, "-nographic", "-no-reboot", "-bios", firmware, "-kernel", str(kernel)]
    started = time.monotonic()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    threads, data, timed_out = set(), bytearray(), False
    os.set_blocking(process.stdout.fileno(), False)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    try:
        while process.poll() is None:
            for path in Path(f"/proc/{process.pid}/task").glob("*/comm"):
                try:
                    threads.add(path.read_text().strip())
                except FileNotFoundError:
                    pass
            for key, _ in selector.select(0.005):
                data.extend(os.read(key.fd, 65536))
            if time.monotonic() - started >= 180:
                timed_out = True
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                break
        process.wait(timeout=2)
        # poll退出后仍drain串口尾部；不以最后一次select作为完整日志。
        os.set_blocking(process.stdout.fileno(), True)
        data.extend(process.stdout.read())
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        selector.close()
        process.stdout.close()
    text = data.decode(errors="replace")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.with_suffix(".log").write_text(text)
    report = {"command": command, "memory": memory, "cpus": cpus,
              "vcpu_threads": sorted(t for t in threads if "/TCG" in t),
              "returncode": process.returncode, "timeout": timed_out,
              "elapsed_seconds": time.monotonic() - started,
              "kernel": identity(kernel), "qemu": identity(qemu),
              "firmware": identity(firmware) if firmware != "default" else {"mode": "default"}}
    output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    if timed_out or process.returncode or len(report["vcpu_threads"]) != cpus:
        raise RuntimeError(f"QEMU execution failed: {report}")
    if "BoarOS: SMP natural exit pages/stack/table owners returned" not in text or "SMP failure" in text:
        raise RuntimeError(f"missing natural resource closure:\n{text[-4000:]}")
    if cpus == 2 and ("SMP online logical=1 hart=" not in text or
                      "SMP timer preemption, unique execution and FD migration passed" not in text):
        raise RuntimeError("missing independent SMP execution and migration evidence")
    print(f"RV SMP cpus={cpus} RAM={memory} native exit and owner closure passed", flush=True)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--firmware", default="default")
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("build/riscv-smp/runs"))
    args = parser.parse_args()
    for cpus in (2, 1):
        for memory in ("512M", "1G"):
            boot(args.qemu, args.firmware, args.kernel, memory, cpus,
                 args.output / f"{cpus}-{memory}")


if __name__ == "__main__":
    main()
