# 主线双架构 CI

主线 `ci.yml` 在 main push、面向 main 的 PR 和手动触发时执行。
它保护已交付的单核 QEMU 契约，不把 CI 成功解释为所有 Linux 应用、SMP 或实板通过。
RV 保留 Ubuntu 24.04/QEMU 8.2.2 的原回归；LA 使用固定 v11.1.0 源码及已有
LS7A RTC 补丁。所有通用 fixture 明确使用 `INIT_CONFIG=config/init.json`。

## 常驻门禁

| Job | 内容 |
|---|---|
| Shared kernel and runner contracts | allocator、VirtIO transport/queue/block/RNG/net、PCI、TTY/PTY/UART、随机/RTC、epoll/lwIP、ext4 cache/batch/metadata 及 runner/profile 反例 |
| RISC-V full test suite | 原完整架构、scale、可睡眠 I/O、双盘故障/冷重启、真实 musl userland、RNG/环境/network/TTY/PTY 及独立生产栈检查 |
| RISC-V Linux / BoarOS differential ABI | 原 1366 条同 ELF 差分、ELF 尾页/BSS、SQLite DELETE/WAL 和原生 C 编译探针 |
| LoongArch CPU, MM, ABI and platform | 两种 RAM 的启动、guard/权限、信号/线程/创建 OOM、FPU/SIMD、动态 musl/TLS/exec 错误、PCI/root、1366 条同 ELF 差分、栈及平台组合 |
| RISC-V original glibc 2.44 | 原五形态 GNU 消费者，版本与二进制身份保持原清单 |
| LoongArch original glibc 2.42 and SQLite | 原五形态 GNU 消费者，以及静态/动态 SQLite DELETE/WAL、多进程和独立重启 |

两个独立 environment job 生产各自的缓存；LA 准备失败不阻止 RV 原回归或 RV GNU 环境。
`Main dual-architecture gate` 使用 `always()` 检查所有声明 job；failure、cancelled、
skipped 或缺失都失败。这个汇总检查可供仓库 required-check 规则使用；workflow
本身不修改 GitHub 分支保护设置。

`tests/ci/suites.json` 是组合清单，`tests/ci/run.py` 执行各组并保存完整输出、
argv、预算、退出码、耗时和最终状态。独立组失败后继续收集下一组；被取消的
清单保持 incomplete，不补记未执行项。超时停止整个进程组，资源回收标为
unverified；正常通过仍由各 native target 的协议、退出及 owner 断言决定。

```sh
make test-ci-host
python3 -B tests/ci/run.py --arch host --suite host
python3 -B tests/ci/run.py --arch loongarch --suite core
python3 -B tests/ci/run.py --arch riscv --suite platform
python3 -B tests/ci/run.py --arch loongarch --suite platform
python3 -B tests/ci/run.py --arch loongarch --suite runtime
```

## 固定环境与缓存

`tests/ci/tools.py` 从 `references/sources.tsv` 选择原工具包。LA 使用 Loongson
2025.08.08 的 GCC 15.1.0/binutils 2.45/glibc 2.42；RV GNU 使用 Arch 固定 GCC
16.2.1/binutils 2.47/glibc 2.44/API headers 7.2 包，与原已安装文件身份一致。
包的 URL 和 SHA-256 只在参考清单维护，不从 latest 或本机缓存猜测输入。

工具安装在 `build/tools/<arch>`。`BOAROS_GLIBC_ROOT` 只迁移宿主文件位置；
原 GNU manifest 的工具、loader/libc/libgcc 哈希、版本以及 LA 安装树校验继续成立。
RV 的 sysroot 指向包内 `usr/riscv64-linux-gnu`，保留原 libc 链接脚本和 CRT，
不改库内容。整个安装的内容、权限和链接目标另有缓存指纹。

RV 原回归仍用 Ubuntu 的交叉工具；平台测试先从固定源码构建完整 BusyBox，
所需 Linux 6.6 UAPI 与测试源码均按参考清单恢复。编译 runner 检测实际 wrapper
是否支持 `-fno-link-libatomic`，不把本机新 GCC 的选项强加给旧工具。环境 shell
的 ripgrep 依赖显式安装；准备日志及 BusyBox 身份与测试输出一起上传。

LA 的 QEMU wrap 依赖按固定源码 `.wrap` revision 恢复，派生模拟器仍核对源码、
补丁、配置和产物。Linux 在任何 make 之前拒绝缺失或损坏的已登记 ELF；不能
通过重复构建给损坏文件重新登记身份。输入或产物变化使缓存验证失败。

environment cache key 包含工作区位置、固定来源清单、准备脚本与 profile、宿主
工具哈希和 Python 身份。只使用精确 key；consumer 必须命中本次 producer 的
完整缓存。工具缓存先独立保存，避免后续内核准备失败导致再次下载同一工具包。
`build/tools`、QEMU/Linux 和用户环境是可复用缓存，`build/ci-run` 是临时输出。

## 完整程序与恢复

`native-extended.yml` 每日北京时间 03:17 和手动运行 RV/LA 两侧，各 job 独立。
它在两种 RAM 使用完整原程序清单的 `--require-pass`，并执行真实终端应用、
Ethernet/TAP、RT 双盘负载或 LA 设备构造/停止/reset/RTC 组合。清单生成成功
不能代替原程序成功。结果、完整日志和失败状态分别上传。

`recovery.yml` 保留现有每周和手动 RV NBD DELETE/WAL 全恢复矩阵。LA 正常
SQLite/重启和块错误门禁不冒充该全断电矩阵，新增 LA 恢复接口需独立验收。
所有 CI 输出核对后通过 `make prune-build` 清理，固定包与复用缓存保留。
