# RV 官方测例运行与评分

本模块仅存在于 `oscomp-rv-compat`。通用能力来自 main；本分支独立保留
uname 4.15.0、启动配置和评测入口，不整体回合主线。这里只跑 RV，不能当作双架构比赛交付。

## 固定输入

`tests/oscomp/inputs.json` 固定 `pre-20250615` 发布镜像及压缩包的 SHA-256，
以及 `references/oscomp-autotest` commit
`d1bb3a3c4b27274e196a2648518525c1a304e339`。四个发布资产长期保存于该目录，
缺失或校验失败直接报错，不重新下载、不修改原盘。LA 只校验身份，不启动。
来源为清单记录的 GitHub release 与 Harness；运行依赖包括项目工具链、QEMU、
e2fsprogs、Python 3.11+、jinja2、pytz，原 judge 使用宿主 Python。

## 启动和用户态环境

`init.json` 将 PID 1 配成原盘 `/musl/busybox sh /boaros-start.sh`。
runner 从原盘建立可丢弃副本，只增加自己的启动脚本。原测试脚本、二进制、权限及
判断逻辑均保留。内核没有比赛路径或调度特判。

启动脚本通过原 BusyBox 创建 `/bin /lib /tmp /dev`、工具与加载器符号链接，
通过 mknodat 创建 null/zero/console。`/tmp` 是 ext4 目录，不是 tmpfs。
每组在自己的 libc 根目录执行原 `*_testcode.sh`，分别设置 `LD_LIBRARY_PATH`，
避免同时搜索两套 libc。musl 的普通/sf 加载器名指向镜像自带 libc；
glibc 加载器指向其真实文件。没有假 `/proc`、随机设备或测试输出。

默认顺序为 basic、busybox、cyclictest、iozone、iperf、libcbench、libctest、
lmbench、ltp、lua、netperf，每组先 glibc 后 musl。无逐组超时、重启或失败后宿主拼接。

## 重建与评分

```sh
python3 -B tests/oscomp/run.py --output build/oscomp-rv-baseline
# 明确标为诊断，不能合入正式总分：
python3 -B tests/oscomp/run.py --output build/oscomp-rv-diagnostic --diagnostic-timeout 60
make all                         # 恢复默认 /init 配置
make test-init-config-riscv       # 交替重建检查，结束后恢复默认
```

runner 读取固定 Harness `kernel/judge/config.json`。其中 `qemu.timeout=3600`；
60 秒只是 `run_qemu.py` 的缺省值，不能冒充随附配置。RV 参数按该源码取 `smp`、
`mem`（默认 1 hart/1G）、VirtIO block/net、user net、RTC UTC、OpenSBI default。
本 profile 无第二盘，不伪造不存在的输入；总时间预算覆盖一次 QEMU 启动。

原 `parse_serial_out_new` 和 22 个 judge 原样运行，未到达组也由原 judge 产生结果。
原 `postwork.postwork` 接收仅含 RV 的 summary；LA 未运行，不在输入中伪造成绩。
其整数分数为 RV 投影，保留原始分组分数与 LTP 变换，不自行重写总分公式。
每组状态区分未到达、超时、脚本失败和已结束；已结束不意味着该组所有测试通过。

`report.json` 保存提交/脏状态、内核和输入哈希、配置、QEMU 命令/版本、一次启动计数、
退出原因、22 组细目及原 postwork 分数。`judge.json` 为原解析器结果，`serial.log`
为完整串口，`identity.json` 在启动前写入，方便中断后定位。HTML 是原 postwork 展示，
可能带有 LA 空列，不能把它解释成跑过 LA。输出只允许放在新建的 `build/` 子目录。

## 证据边界与清理

历史逐组重启、修改启动方式的成绩仅作诊断；官方脚本是否启动、环境缺口和内核 ABI
错误分别记录。镜像 basic 的 `run-all.sh` 为 0644，原包装器却直接执行它；本入口
保留该行为，不能据此把 basic 的低分全部归为内核缺少对应 syscall。

评分核对后将结论、运行身份和重建命令写入本文，运行产物由 `make prune-build`
清理。原始 `.img/.img.xz` 在 references，不属于清理范围。通用新缺陷先最小复现，
回 main 修复并验收，再 merge 回本分支重新构建运行。
