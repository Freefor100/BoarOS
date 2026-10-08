# 将本机验收环境迁入主线 CI

2026-10-08 的调整针对主线已经交付的单核 RV/LA 能力。旧 CI 的两个 job
实际只有 RV；增加 `make test-loongarch` 也只覆盖其首阶段聚合依赖，不能替代
后来独立交付的 SIMD、动态 TLS、PCI、终端和设备生命周期门禁。
当前执行组合和入口见[CI 模块](../modules/continuous-integration.md)。

## 同版本仍不足以复现 GNU 验收

原 GNU profile 绑定本机绝对路径和文件身份：RV glibc 2.44、LA glibc 2.42。
Ubuntu 安装同名交叉编译器不会自动满足这些二进制、CRT、headers 或安装树。
实际查得 LA Loongson 2025.08.08 发布包与既有 GCC 15.1.0 和 glibc manifest
全部身份一致；RV 原安装来自 Arch GCC 16.2.1-1、binutils 2.47-1、glibc 2.44-1
和 Linux API headers 7.2-1。原包恢复后再次核对固定 GNU manifest，保留原字节。

LA 发布依据为
[Loongson release 2025.08.08](https://github.com/loongson/build-tools/releases/tag/2025.08.08)，
其资产 API SHA-256 为 `b8572e2083143ff1807658f02e11eba53e5ed81d6194854d369b43fceea72de7`。
RV 包取自 [Arch Linux package archive](https://archive.archlinux.org/packages/r/)，
逐包 URL/SHA-256 归 `references/sources.tsv`，访问日期均为 2026-10-08。
安装位置迁移后仍检查 release 字符串、工具/runtime 哈希、内容、权限和链接。

RV 包的 GNU headers 位于 target 的 `usr/include`，Linux headers 另位于 target
的 `include`；原 libc 链接脚本使用 `/lib` 与 `/usr/lib`，因此 sysroot 必须是
`<package>/usr/riscv64-linux-gnu`，不能误用整个包目录。迁移后的两侧五形态
GNU 程序已实际通过固定 Linux/BoarOS，LA 含两种 RAM。Ubuntu 24.04 容器能
运行两侧原编译器并编译原 RV GNU probe；这些本地结果和首次托管结果分列。

## 缓存不能重新确认自身损坏

旧 LA 准备器在命中缓存后执行 make，再把当前 vmlinux 的哈希写回身份文件。
如果已损坏的 ELF 时间较新，make 不会重链接，却会给它登记新身份。host 反例
分别复现损坏和缺失 ELF：现在在调用 make 以前核对既有 image_sha256，失败
明确拒绝。固定依据仍为 `references/linux@f4cdf7ca9a1fdcca413157df19753f388a5a224e`
与 `references/qemu@84f07211cc5b4fc6a371559bf8a5de4fb068e648`。

共用 CI runner 保留命令的完整输出并分别记录结果；一项失败继续下一独立组，
不是把失败覆盖成最后一项成功。timeout、缺工具、缺失 job 和 skipped 都不能
通过汇总；取消时未执行项保持 incomplete。原程序清单必须显式严格判定。

整体审查另确认外层缓存必须包含 BusyBox 输入清单、RV Linux builder/config
及其宿主工具身份；否则正常更新会恢复旧 key 并被内部身份拒绝，或新内层
产物无法重新保存。已用两种架构的输入变化反例保护新 key。producer 本身
无论成功失败都上传准备日志/身份；consumer 按实际 network、TTY/PTY 和
原程序运行目录归档完整输出，失败镜像单列，不能只依赖聚合日志中的尾部。
