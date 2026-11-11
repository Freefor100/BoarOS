# 原 BPF 工具消费者

`build.py` 从 `references/sources.tsv` 读取固定输入，原样构建 fixed Linux
源码中的 bpftool/libbpf，以及 bpftrace 和其固定 libbpf。缓存、解包源码和
日志均位于忽略的 `build/bpf-20261107/`，不改写 `references/linux/`。
RV 使用 glibc 2.44，LA 使用 LP64D/glibc 2.42；编译器与仓库已有 GNU
工具链缓存相同。LLVM 的本机 tblgen、flex 和 bison 只生成源码，不替代
客机里的 bpftrace 编译执行。

```sh
python3 -B tests/tools/test-bpf-inputs.py
python3 -B tests/workloads/bpf/build.py --arch riscv --stage all
python3 -B tests/workloads/bpf/linux.py --arch riscv
python3 -B tests/workloads/bpf/baseline.py --arch riscv
```

LA 将 `--arch` 改为 `loongarch`。默认构建并行数为 2、QEMU 为单 CPU／1 GiB。
`--stage` 可单独继续已开始的依赖构建；`--mode bpftool` 先检查 bpftool 与
libbpf UDP 消费者，默认 `full` 还执行完整 bpftrace。

Linux 使用专用 BPF/BTF/perf/tracefs 内核配置，RV 开启 SMP／8 CPU 上限，
LA 保留 SMP 和 16 KiB 三级页表；不复用或覆盖其他 Linux profile。
默认启动 1 CPU，`baseline.py --cpus 2`（或 8）选择对应的真实多核启动。成功运行后删除一次性镜像与
内核副本，保留日志、输入身份和可复用构建缓存；失败镜像保留供检查。

`baseline.py --platform boaros --kernel /absolute/kernel/path` 复用同一个
客机树，并先比较对应 CPU 数的成功 Linux 记录中的 ELF、脚本和运行库 SHA-256。
程序身份与实际 QEMU 命令记录在 `<arch>/baseline-<mode>/result.json` 或
`<arch>/boaros-<mode>/result.json`；2／8 CPU 的目录追加 `-2cpu`／`-8cpu`。
`--output PATH` 可为另一份生产内核保留独立结果，仍核对对应 CPU 的 Linux 输入。

`udp.bpf.c` 包含 BTF map 与只读全局变量；`udp.c` 使用原 libbpf 加载它，
向实际 UDP socket 安装过滤器，验证丢弃、修改 map 后允许接收，以及释放
socket/program/map。bpftool 另检查 ELF load、prog/map/btf show、map
update/lookup/dump 和 bpffs pin/get/unlink，unlink 后再 GET 确认路径已释放。
不要求全局 ID 列表瞬间为空：原 bpftool 的 PID iterator 会创建临时 BPF 对象，
Linux 允许其在延迟释放期间短暂出现在枚举结果中。

bpftrace 在客机编译 `syscalls.bt`。外部目标先阻塞在 FIFO；原 bpftrace
完成 tracepoint attach 后消费 BEGIN 的 ringbuf 事件，控制器收到该事件才
许可目标执行 64 次 getpgid。脚本按 PID/TID 检查进入／退出次数、配对延迟
及 ringbuf printf。SIGINT 场景在目标确认全部 64 次调用完成、控制器读取全部
64 条实际 ringbuf 输出后中断 tracer，
再释放并回收目标，检查 END 输出。此路径使用原 `-p` 与
位置参数，不要求 `-c` 附带的 PR_SET_PDEATHSIG/ptrace 行为。
回收目标后的 FIFO 排空使用 shell 内建循环，避免另起进程立即复用目标 PID，
使原工具基于 `kill(pid, 0)` 的存活检查误认目标仍在运行。

## 原生 API／并发对照

`abi.py` 复用已有 `arch_profiles`、fixture 和客机执行器，把同一个静态
glibc ELF 运行在固定 Linux 与 BoarOS 上。`bpf_socket`、`bpf_trace`、
`bpf_test_run`、`bpf_mmap` 分别检查 socket 过滤、tracepoint、TEST_RUN 和
真实映射／远程 CPU 的状态与释放；失败保留失败结果，不接受两种不同结果。

2／8 CPU 的 BoarOS 对照需要实际 USER_SMP 生产内核，不能把默认 UP 内核
加上 QEMU `-smp 8` 当作并发执行：

```sh
make -j2 SMP=1 USER_SMP=1 SMP_CPUS=8 all
make -j2 LA_SMP=1 LA_SMP_CPUS=8 kernel-la
python3 -B tests/workloads/bpf/abi.py --arch riscv --cpus 1 2 8 \
  --workload bpf_socket --workload bpf_trace \
  --workload bpf_test_run --workload bpf_mmap \
  --kernel build/riscv-user-smp8/kernel-rv --output build/bpf-api-rv
```

LA 使用 `--arch loongarch --kernel build/loongarch-smp8/kernel-la` 和独立
输出目录。客机 supervisor 在两种内核实际挂载 proc/sysfs/tracefs，并检查
子程序退出状态及 BoarOS 的 root 资源归还。

已有成功的 Linux 结果时，可追加 `--only boaros --reference LINUX_OUTPUT`。
执行器先核对 source／ELF SHA、每个 workload／CPU 的 Linux PASS，再复制
原 ELF 字节，避免第二次编译改变比较输入。每轮 `result.json` 记录 source、
ELF、kernel、Linux config 的 SHA 和实际命令；输出目录必须尚不存在。
