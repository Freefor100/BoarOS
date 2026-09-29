# 会话与调度的真实消费者窄诊断

本诊断不计比赛分数，不修改原程序 ELF，也不据单个退出码宣称应用完整支持。
固定输入先经过 SHA-256 校验，再把同一 fixture 分别交给单 hart Linux 和 BoarOS。

## 重建

先按 `docs/modules/program-environment.md` 构建完整 BusyBox 环境，再运行：

```sh
python3 tests/session-consumer-diagnostics.py \
  --output build/session-consumers-check --kernel kernel-rv
```

输出目录必须不存在。可用 `--linux-kernel <Image>` 选已有固定 Linux 镜像，
runner 会验证其上级 `identity.json`；`--only linux` 或 `--only boaros` 可缩小重跑。
需要本机 QEMU 11 插件头、C 编译器、glib 开发文件、debugfs 和现有 RV64 musl 工具链。
这里的 guest driver 使用 `tests/workloads/diagnostics/session-consumers.c`，
QEMU observer 使用同目录 `syscall-entry-plugin.c`。

观察器仅记录 `priv == U` 的 RV64 `ecall` 入参，不修改寄存器、内存或返回值。
每个诊断前后的负 PID `getpgid` 标记划分调用链；它们不属于原消费者。
`report.json` 保存输入、driver、观察器、kernel 的哈希、QEMU 版本、命令、
串口记录和分用例入口计数，`*.ecall` 保留原始入口。
入口不是成功返回的证明：同时检查消费者错误输出、后继程序查询及循环计数。
观察器不跟踪 syscall 返回值；首次失败的 errno 来自原程序错误输出。

## 固定资料与输入

- Linux：`references/linux`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。
- QEMU 插件接口参照 `references/qemu` v11.1.0 的
  `include/plugins/qemu-plugin.h`、`contrib/plugins/execlog.c` 和
  `gdbstub/gdb-xml/riscv-64bit-virtual.xml`；实跑 QEMU 11.1.1。
- 完整 BusyBox 来源 `references/oscomp-testsuits` revision
  `b5ec6ef8497e1818cbdec3b54bb722f036e57972`，构建 manifest 在
  `build/program-environment/full-busybox/build.json`；ELF SHA-256
  `f2cda5fcdff6d41c8a553ac658e8aa55b6a48aa40898cb123a19f7865f3773ac`。
- 原镜像 `references/oscomp-autotest/sdcard-rv.img` SHA-256
  `f419468678d342133546add2f8459ea09aeba987ba968e28753d6ee656996b8b`；
  `/musl/iperf3` SHA-256
  `8a87cda6b79966699bc68c72894288ba9aa4a5d44dc0036cfb257a4a4d73860b`；
  `/musl/cyclictest` SHA-256
  `79e6cf0b469168fb1aae66db6904b839544d5b1fd8a46272fd32943a0db66835`。

## 2026-09-29 最终快照证据

BoarOS kernel SHA-256
`be5ca22629c904a427241b0f92e9d561d0312952e787ab75870ec4beae0143b3`，
Linux Image SHA-256
`a8b79593d1beb2acbd6be5c89062d1e924f14d1f502a026acae2d1de2b060d74`；
driver ELF SHA-256
`9d343a3bf4036e62ab9a7d7bef57ca2e1e85f1bd0c2021a3658978e1a39d87d4`。
最终快照以同一 driver 和固定 Linux 重跑，相关 syscall 入口计数与初版一致。
两边七条独立调用链全部到达 `PROBE complete`，BoarOS 最终 heap-live=0。

| 消费者 | 两边实际入口与观察结果 | 后续限制 |
| --- | --- | --- |
| BusyBox `setsid /init endpoint` | 157；exec 后 PID=PGID=SID | 无 TTY 覆盖 |
| BusyBox `chrt -f 10 /init endpoint` | 119；exec 后 policy=1、priority=10 | 不是负载公平性测试 |
| BusyBox `chrt -r 10 /init endpoint` | 119；exec 后 policy=2、priority=10 | 不是 RR 延迟测量 |
| BusyBox `taskset 1 /init endpoint` | 122、123；exec 后 CPU0 有效 | 单 hart |
| libc `daemon(1,1)` | 157；返回 0，第二次 fork 后 SID=PGID≠PID | 独立 musl probe，非原 iperf 的 daemon 模式 |
| 原 `iperf3 -c 127.0.0.1 -A 0 -t 1` | 122 后到 socket198/connect203 | 无服务器/网络设施；Linux ENETUNREACH，BoarOS ECONNRESET，均 exit1；不证明吞吐支持 |
| 原 `cyclictest -q -t1 -p10 -i1000 -l10` | 119 四次、120/121 各一次、123 四次；线程报告 C:10，exit0 | 两边缺 cpu_dma_latency；Linux 非高精度定时器警告；BoarOS mlock228 报 ENOSYS 后继续。无性能结论 |

环境必须提供 `/dev/urandom` 和 `/dev/shm`；缺失它们会在新增接口之前或循环之前
失败。BusyBox 自识别依赖 basename，fixture 名称使用 `/busybox`。
cyclictest 的 `shm_open` 错误也可能 exit0，必须检查真实 `C:` 输出。
构建 driver 的既有 musl `sched_getscheduler/getparam` 包装未发 syscall 就返回 ENOSYS；
endpoint 用明确的 `syscall(SYS_sched_*, ...)` 查询，避免把 libc stub 误判成内核失败。
Linux 裸 PID1 的 PGID/SID 是 0，BoarOS 为 1；这里比较关系与策略，不比较分配的数字。

原始运行目录是可清理证据，不是永久档案；上述输入身份和命令才是重建依据。
