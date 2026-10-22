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

def observe_threads(pid):
    names=set()
    for path in Path(f'/proc/{pid}/task').glob('*/comm'):
        try:names.add(path.read_text().strip())
        except (FileNotFoundError,ProcessLookupError):pass
    return names


def boot(qemu, firmware, kernel, memory, cpus, output, profile="normal", contract="scheduler", disks=(), require_fpu_setup=False):
    command = [qemu, "-machine", "virt", "-accel", "tcg,thread=multi",
               "-name", "boaros-smp,debug-threads=on", "-smp", str(cpus),
               "-m", memory, "-nographic", "-no-reboot", "-bios", firmware, "-kernel", str(kernel)]
    for index,disk in enumerate(disks):
        command += ["-drive",f"file={disk},if=none,format=raw,cache=writeback,id=d{index}",
                    "-device",f"virtio-blk-device,drive=d{index},bus=virtio-mmio-bus.{index}"]
    if disks: command += ["-global","virtio-mmio.force-legacy=false"]
    started = time.monotonic()
    disk_identities=[identity(disk) for disk in disks]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    threads, data, timed_out = set(), bytearray(), False
    os.set_blocking(process.stdout.fileno(), False)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    try:
        while process.poll() is None:
            threads.update(observe_threads(process.pid))
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
    report = {"command": command, "disks_before":disk_identities, "memory": memory, "cpus": cpus,
              "profile": profile,
              "contract": contract, "require_fpu_setup":require_fpu_setup,
              "vcpu_threads": sorted(t for t in threads if "/TCG" in t),
              "returncode": process.returncode, "timeout": timed_out,
              "elapsed_seconds": time.monotonic() - started,
              "kernel": identity(kernel), "qemu": identity(qemu),
              "firmware": identity(firmware) if firmware != "default" else {"mode": "default"}}
    output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    if timed_out or process.returncode or len(report["vcpu_threads"]) != cpus:
        raise RuntimeError(f"QEMU execution failed: {report}")
    if contract == "mm-disk" and profile!="normal":
        validate_mm_failure(text,profile,contract)
        print(f"RV mm-disk profile={profile} RAM={memory} retained owners; recovery unverified",flush=True)
        return text
    if contract in ("mm-disk","files","file-io"):
        required=("disk MM CPU0 I/O/cache/alias/mount owners returned",
                  "file MM stale preparation/EOF/regrowth/private COW passed",
                  "MM arena closure live=0x0 heap-pages=0x0",
                  "SMP root overlap counter=", "controller/arena/wait owners returned",
                  "PID 1 exited status=0x2a", "heap-live=0x0; shutting down",
                  "disk MM per-page write pin/native RFENCE/other-page/msync snapshot passed",
                  "disk MM deferred snapshot/cold progress/CPU0 worker writeback passed",
                  "file MM real ext4 cross-root/source/registration/pin/RFENCE owners passed")
        if any(marker not in text for marker in required) or "SMP failure" in text or "SMP fatal" in text:
            raise RuntimeError(f"missing native disk MM closure:\n{text[-5000:]}")
        if contract in ("files","file-io"):
            if ("FILES SMP native FD reuse/early wake/EOF/cancel/content/task rollback passed" not in text or
                "FILES SMP root overlap packets=" not in text or
                "FD/OFD/pipe/wait owners returned" not in text or
                "BoarOS: PID 1 exited status=0x2a" not in text):
                raise RuntimeError(f"missing native FD/pipe/root closure:\n{text[-5000:]}")
        if contract=="file-io":
            caller=f"0x{1 if cpus==2 else 0:x}"
            for marker in ("FILE IO native cold buffer/offset/prefix/sync/close owners passed",
                           "BLOCK caller reads hart="+caller,
                           "BLOCK caller writes/flush hart="+caller,
                           "EXT4 direct backend read hart="+caller,
                           "FILE IO caller metadata/submit hart="+caller,
                           "FILE IO cold mmap backend/submit hart="+caller):
                if marker not in text: raise RuntimeError("missing caller file I/O evidence: "+marker)
        if contract=="file-io" and f"FILE IO local hot copy hart=0x{1 if cpus==2 else 0:x}" not in text:
            raise RuntimeError("missing actual caller-hart hot copy evidence")
        for disk in disks:
            subprocess.run(["e2fsck","-fn",str(disk)],check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        print(f"RV {contract} cpus={cpus} RAM={memory} actual I/O and owner closure passed",flush=True)
        return text
    if contract in ("mm", "mm-shared", "mm-file"):
        if profile != "normal":
            validate_mm_failure(text, profile, contract)
            print(f"RV {contract} profile={profile} RAM={memory} expected fault; recovery unverified", flush=True)
            return
        required = ("MM SMP native pin/permission/retirement/U-execute contracts passed",
                    "MM arena closure live=0x0 heap-pages=0x0",
                    "MM SMP natural exit arena/MM/pin/table/stack/wait owners returned")
        if contract in ("mm-shared", "mm-file", "mm-disk"):
            required += ("shared MM cross-root/fault/protected/migration/RFENCE owners passed",)
        if contract == "mm-file":
            required += ("file MM stale preparation/EOF/regrowth/private COW passed",
                         "file MM real tmpfs cross-root/source/registration/pin/RFENCE owners passed",
                         "file MM controller/OFD/registration/private mount owners returned")
        if any(marker not in text for marker in required) or "SMP failure" in text or "SMP fatal" in text:
            raise RuntimeError(f"missing native MM closure:\n{text[-4000:]}")
        if cpus > 1 and any(f"SMP online logical={i} hart=" not in text for i in range(1,cpus)):
            raise RuntimeError("MM test did not publish the secondary CPU")
        print(f"RV {contract} cpus={cpus} RAM={memory} native exit and owner closure passed", flush=True)
        return
    if profile != "normal":
        expected = {"missing-rfence": "SMP failure stage=SBI-extension",
                    "denied-start": "SMP failure stage=HSM-start",
                    "handshake-timeout": "SMP failure stage=start-timeout",
                    "guard": " cause=0xd pc=", "denied-stop": "SMP failure stage=stop-confirmation",
                    "rfence-error": "SMP failure stage=RFENCE"}[profile]
        if expected not in text or "SMP natural exit" in text:
            raise RuntimeError(f"negative profile did not reach its failure boundary:\n{text[-4000:]}")
        if profile in ("missing-rfence", "denied-start", "handshake-timeout") and f"SMP online logical={1 if profile=='missing-rfence' else cpus-1} " in text:
            raise RuntimeError("failed startup published ONLINE")
        if profile == "rfence-error" and "SMP RFENCE error retains table and page owners" not in text:
            raise RuntimeError("RFENCE error reached without retained-owner evidence")
        print(f"RV SMP profile={profile} RAM={memory} expected fault; recovery unverified", flush=True)
        return
    if "BoarOS: SMP natural exit pages/stack/table owners returned" not in text or "SMP failure" in text:
        raise RuntimeError(f"missing natural resource closure:\n{text[-4000:]}")
    if cpus > 1 and ("SMP online logical=1 hart=" not in text or
                      "SMP timer preemption, unique execution and FD migration passed" not in text or
                      "SMP early wake, WFI boundary and remote deadline passed" not in text or
                      "SMP delayed RFENCE return retains table and page owners" not in text or
                      "SMP dual CPU eligibility, SP and FD ownership passed" not in text or
                      "SMP join/retirement borrow and CPU0 collection passed" not in text or
                      "SMP RW reader batch, writer FIFO and qualification owners passed" not in text or
                      "SMP per-CPU FIFO/yield and RR/timer qualification passed" not in text or
                      "SMP sleeping create rechecks STOPPED and returns private owners" not in text):
        raise RuntimeError("missing independent SMP execution and migration evidence")
    if require_fpu_setup and cpus==2 and "FPU setup interrupt boundary protected; 8 owners returned" not in text:
        raise RuntimeError("missing forced FPU setup boundary/owner observation")
    if any(f"SMP online logical={i} hart=" not in text for i in range(1,cpus)):
        raise RuntimeError("a requested secondary was not online")
    print(f"RV SMP cpus={cpus} RAM={memory} native exit and owner closure passed", flush=True)


def validate_mm_failure(text, profile, contract="mm"):
    required = {
        "rfence-error": ("SMP failure stage=MM-RFENCE", "MM RFENCE error retains data/table batch owners"),
        "mm-fence-i": ("SMP failure stage=MM-FENCE.I", "MM FENCE.I error retains live MM/code owners"),
    }
    if contract in ("mm-shared", "mm-file", "mm-disk"):
        if profile != "rfence-error": raise ValueError("unsupported shared MM failure profile")
        required[profile] = ("SMP failure stage=MM-RFENCE", "shared MM RFENCE error retains object/pin/data/table owners")
    elif contract != "mm": raise ValueError("unsupported MM failure contract")
    if contract in ("mm-file","mm-disk"):
        required["rfence-error"] += ("file MM RFENCE error retains source/registration/data/table owners",)
    if contract=="mm-disk":
        required["rfence-error"] += ("disk MM RFENCE error retains cache/mount/source/data/table owners",)
    if profile not in required:
        raise ValueError("unsupported MM failure profile")
    if any(marker not in text for marker in required[profile]) or "natural exit" in text:
        raise RuntimeError(f"MM failure lacks boundary/retained-owner evidence:\n{text[-4000:]}")


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--cpus',type=int,choices=(1,2,3,4),action='append')
    parser.add_argument('--memory',choices=('512M','1G'),action='append')
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--firmware", default="default")
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("build/riscv-smp/runs"))
    parser.add_argument("--profile", choices=("normal", "missing-rfence", "denied-start", "handshake-timeout", "guard", "denied-stop", "rfence-error", "mm-fence-i"), default="normal")
    parser.add_argument("--contract", choices=("scheduler", "mm", "mm-shared", "mm-file", "mm-disk", "files", "file-io"), default="scheduler")
    parser.add_argument("--init",type=Path)
    parser.add_argument("--require-fpu-setup",action="store_true")
    args = parser.parse_args()
    if args.require_fpu_setup and (args.contract!="scheduler" or args.profile!="normal"):
        parser.error("FPU setup boundary belongs to the normal scheduler contract")
    if args.contract in ("files","file-io") and args.profile!="normal":
        parser.error("FD/pipe composition uses the normal profile; RFENCE errors have the existing MM contracts")
    if args.contract in ("mm-disk","files","file-io"):
        if not args.init or args.profile not in ("normal","rfence-error"): parser.error("disk MM requires --init and normal/rfence-error profile")
        for cpus in ((2,1) if args.profile=="normal" else (2,)):
            for memory in (args.memory or ("512M","1G")):
                folder=args.output/f"{cpus}-{memory}"
                folder.mkdir(parents=True,exist_ok=True)
                root=folder/"root.img"; disk=folder/"disk.img"
                for image in (root,disk):
                    subprocess.run(["truncate","-s","32M",str(image)],check=True)
                    subprocess.run(["mkfs.ext4","-q","-F",str(image)],check=True)
                subprocess.run(["debugfs","-w","-R",f"write {args.init.resolve()} /init",str(root)],check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
                subprocess.run(["debugfs","-w","-R","set_inode_field /init mode 0100755",str(root)],check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
                boot(args.qemu,args.firmware,args.kernel,memory,cpus,folder/"first",profile=args.profile,contract=args.contract,disks=(root,disk))
                if args.profile!="normal": continue
                text=boot(args.qemu,args.firmware,args.kernel,memory,cpus,folder/"restart",contract=args.contract,disks=(root,disk))
                if "disk MM independent restart durable content passed" not in text:
                    raise RuntimeError("independent restart did not verify the durable bytes")
                if args.contract=="file-io" and "FILE IO independent restart content passed" not in text:
                    raise RuntimeError("independent restart did not verify ordinary file content")
        return
    if (args.contract in ("mm-shared", "mm-file", "mm-disk") and args.profile not in ("normal", "rfence-error")) or (args.contract == "mm" and args.profile not in ("normal", "rfence-error", "mm-fence-i")) or (args.contract != "mm" and args.profile == "mm-fence-i"):
        parser.error("profile does not belong to the selected contract")
    for cpus in (args.cpus or ((2, 1) if args.profile == "normal" else (2,))):
        for memory in (args.memory or ("512M", "1G")):
            boot(args.qemu, args.firmware, args.kernel, memory, cpus,
                 args.output / f"{cpus}-{memory}", args.profile, args.contract,
                 require_fpu_setup=args.require_fpu_setup)


if __name__ == "__main__":
    main()
