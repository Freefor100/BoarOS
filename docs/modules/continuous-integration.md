# 主线双架构 CI

`ci.yml`在main push、面向main的PR和手动触发时执行，保护已经交付的单核QEMU
契约。两侧使用同一个`native-contracts.yml`执行模板及同一个结果runner；
`tests/ci/suites.json`维护实际命令，不能以job数量或组数推定功能覆盖。

RV保留Ubuntu24.04/QEMU8.2.2回归，LA使用固定QEMU11.1.0及LS7A RTC补丁；
GNU输入仍为RV glibc2.44、LA glibc2.42。这些环境用于架构正确性，不能直接比较性能。
main push等待同组执行收口；PR取消同组旧执行，固定环境冷构建和完整缓存不被反复中断。

## 共同门禁与独立扩展

| 层级 | RV | LA |
|---|---|---|
| 固定环境 | 原GNU工具/runtime及Linux缓存 | 原GNU工具/runtime、QEMU、Linux及musl缓存 |
| core / ABI | 完整RV、scale、I/O等待、双盘、userland、1366 ABI、ELF尾页与栈 | CPU/MM/权限、信号/线程、等待、FPU/SIMD、动态exec、PCI/root I/O、1366 ABI与栈 |
| runtime | glibc五形态、SQLite DELETE/静态动态CLI/独立重启、WAL | 相同消费者与完成条件，保留目标ELF及libc版本差异 |
| platform | RNG/时钟、network、termios2/TTY/PTY | 相同类别，另含boot random和16KiB pipe geometry |
| 定时完整消费者 | 229项原程序、TTY应用、实际Ethernet、RT双盘 | 229项原程序、TTY应用、实际Ethernet、PCI设备失败及RTC模型 |

共同ABI、SQLite、RNG、network和TTY/PTY由CI显式传入`TEST_MEMORIES=512M 1G`；
RV环境脚本分别执行两种RAM。空的`TEST_MEMORIES`保留聚焦入口原来的默认值。
RV DELETE入口复用与LA相同的Linux/BoarOS runner，保留Make提供的实际程序、
loader和kernel路径，不再只运行BoarOS的旧shell smoke。glibc原五形态本身已执行双RAM。

架构专有验证保留实际差异：RV legacy/modern MMIO、NBD断电恢复和客体原生C
编译；LA LSX/LASX、PCI共享INTx/reset和LS7A RTC。LA普通SQLite重启不代替
RV已验证的全断电矩阵；LA客体原生开发尚未交付，不增加占位成功job。

两个environment独立生产缓存，各consumer只依赖本架构producer。core的一组
失败后runner继续收集后续独立组；同模板的platform准备与执行在native环境成功时
继续，不被core失败跳过。runtime与core独立。`Main dual-architecture gate`以
`always()`检查全部声明job，failure/cancelled/skipped/缺失都失败；工作流不改变分支保护。

host allocator组直接链接生产CPU/raw、buddy/slab、等待与RW资格，覆盖4/16KiB
并发、非法owner、抢占、有界工作量及借用销毁。RV完整门禁与LA cpu-state运行
真实raw/等待/handoff，含双RAM首次/恢复/退出切换；宿主并发不代表整个内核SMP-ready。

`tests/ci/run.py`保存完整输出、argv、预算、退出码和最终状态。超时停止整个进程组，
回收标为unverified；取消保留incomplete，未执行项不补成功。host使用宿主PATH和
空LD_LIBRARY_PATH；输出超时测试先确认真实子进程flush/ready，正式计时预算不变。
所有native Make命令显式使用`INIT_CONFIG=config/init.json`。

```sh
make test-ci-host
python3 -B tests/ci/run.py --arch host --suite host
python3 -B tests/ci/run.py --arch riscv --suite core
python3 -B tests/ci/run.py --arch loongarch --suite core
python3 -B tests/ci/run.py --arch riscv --suite runtime
python3 -B tests/ci/run.py --arch loongarch --suite runtime
python3 -B tests/ci/run.py --arch riscv --suite platform
python3 -B tests/ci/run.py --arch loongarch --suite platform
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
补丁、配置和产物。宿主显式安装`libslirp-dev`；普通及RTC派生QEMU在发布/复用前
用`-machine virt -netdev help`核对真实user后端，缺失则环境准备失败。
Linux 在任何 make 之前拒绝缺失或损坏的已登记 ELF；不能
通过重复构建给损坏文件重新登记身份。输入或产物变化使缓存验证失败。

environment cache key 包含工作区位置、固定来源清单、依赖安装action、准备脚本与 profile、宿主
工具哈希和 Python 身份。只使用精确 key；consumer 必须命中本次 producer 的
完整缓存。工具缓存先独立保存，避免后续内核准备失败导致再次下载同一工具包。
`build/tools`、QEMU/Linux 和用户环境是可复用缓存，`build/ci-run` 是临时输出。

## 完整程序与恢复

`native-extended.yml` 每日北京时间 03:17 和手动运行 RV/LA 两侧，各 job 独立。
它在两种 RAM 使用完整原程序清单的 `--require-pass`，并执行真实终端应用、
Ethernet/TAP、RT 双盘负载或 LA 设备构造/停止/reset/RTC 组合。清单生成成功
不能代替原程序成功。结果、完整日志和失败状态分别上传。

运行前独立完成输入准备：RV用`make prepare-program-environment`建立musl及UAPI，
`tests/network_inputs.py`核对并恢复原发布盘的网络运行时；两侧先执行原程序runner的
`--build-only`，实际清单再以`--reuse-builds`验证并复用这些产物。这样TTY不依赖前一组
偶然留下的BusyBox，准备失败也不会冒充客户机程序失败。

TAP预检实际创建私有user/net namespace、执行TUN ioctl及IP配置，不以`unshare true`
代替设备权限验证。`tests/ci/netns.py`默认只检查；仅明确的GitHub CI配置且宿主启用
AppArmor userns限制时，对专用`unshare`副本加载带userns许可的profile，再重复实际预检。
网络runner通过`BOAROS_UNSHARE`使用该副本；测试、QEMU和产物保持普通job UID，
不全局关闭限制或把整套测试提到root。probe超时停止整个进程组，workflow的always步骤
卸载临时profile；仍失败就保留环境错误。依据为[Ubuntu官方说明](https://discourse.ubuntu.com/t/understanding-apparmor-user-namespace-restriction/58007)，访问2026-10-10。

严格清单同时检查原断言和两侧结果。固定Linux的原BusyBox `du`可能因遍历中的`/proc/PID`
消失返回失败，脚本整体退出0也不能改记成功；此类结果保持`reference-not-pass`并令门禁失败。
被排除、未到达或仅本地通过的项目不能补成托管CI通过。

```sh
make prepare-program-environment
python3 -B tests/network_inputs.py
python3 -B tests/program-inventory/run.py --arch riscv --build-only
python3 -B tests/ci/netns.py
python3 -B tests/ci/run.py --arch riscv --suite extended
```

LA准备沿native-environment的固定入口；原程序`--build-only`使用`--arch loongarch`。
本地命名空间检查不修改host policy，GitHub专用配置由workflow明确调用。

`recovery.yml` 保留现有每周和手动 RV NBD DELETE/WAL 全恢复矩阵。LA 正常
SQLite/重启和块错误门禁不冒充该全断电矩阵，新增 LA 恢复接口需独立验收。
所有 CI 输出核对后通过 `make prune-build` 清理，固定包与复用缓存保留。
