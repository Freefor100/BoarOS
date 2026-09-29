# 单 hart 可睡眠存储与跨层 owner

2026-09-28，从 `9ac4947` 的单核规模基线接入运行期 IRQ 等待。同步 block/VFS API、用户 ABI、磁盘日志格式与 ordered data → log → commit → checkpoint 顺序保持不变。设计采用 OFD、namespace、inode 与后端分别同步；不用覆盖整个存储调用的全局锁。纯后端读共享进入，写事务独占；不冲突的缓存命中仍可完成。

## 固定依据与平台边界

- `references/qemu/hw/block/virtio-blk.c`、`hw/virtio/virtio-mmio.c`、`hw/riscv/virt.c`，QEMU v11.1.0 commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`：feature/cache mode、virtqueue、PLIC 路由、同步 reset 与请求合并。实际测试 QEMU 11.1.1。
- `references/linux/drivers/block/virtio_blk.c`、`fs/read_write.c`、`fs/file.c`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`：FLUSH 缺失时的 write-through、定位/共享 offset 接口参考。
- `references/riscv/riscv-privileged-20260120.pdf`，版本 20260120，SHA-256 `d0f818af6fa519d39e68f822aa795bff9f38032a2f352afdf43e91c0d480e408`：supervisor external interrupt 与本地地址空间契约。来源固定于 `references/sources.tsv`。

范围仅 RV64、QEMU virt、单 hart、legacy/modern VirtIO。QEMU 同步 reset 必须读回零后才允许释放 DMA owner；异步实板 reset、SMP、多写事务、后台写回和事务合并均未实现。

## 所有权与失败路径

每任务有同步/分配/后端上下文。锁顺序 OFD offset → namespace → 稳定身份排序的 inode → backend，写者排队后阻止后续读者。内核存储 wait 不可被信号拆除；pending 退出在请求完成或 reset 之后，沿正常调用栈释放资源，再返回用户边界处理。任务退出后的存储清理由可调度内核任务承担，idle/IRQ 不进入可睡眠路径。

VirtIO 使用最多 32 描述符、8 个在途槽。准备不发布 DMA；提交后槽的任务 owner、header/status、bounce 与描述符一直保留至确认完成。IRQ 只收割/ack/唤醒；队列满则等待。flush 阻止新提交、排空此前请求，再完成设备或软件屏障。timeout 先禁止提交并 reset，确认停止 DMA 后一次性完成失败请求；设备保持失败态。统计的 max_inflight 计已发布且未确认完成的请求，不计仅预留槽。

缓存同页 miss 合并为一个 loader，不同页并发；写回固定页快照和 dirty generation。writeback 选页数组在首次睡眠前逐项 pin，之后只释放原集合。inode 写锁排除 truncate/失效/orphan 最后 close 与 loading/writeback。用户复制在存储锁外；文件/ELF 缺页先 pin source 和 VMA generation，I/O 返回后验证版本与同地址 PTE，再发布。另一任务先完成同页缺页可满足本次请求。

lwext4 纯读使用局部游标 `ext4_fpread`；同块缓存 loading 合并，不同块可同时等待。事务记录任务 owner，只有同 owner 可嵌套；orphan、journal 生命周期、恢复和卸载也持独占 gate。持锁、堆与 MM 元数据分配只回收干净页；正常干净回收立即释放最后 node（`ext4_fclose` 仅清理内存）；只有未完成 orphan/真实失败保留 mount owner。脏回收在外层无锁处执行，真实耗尽返回 ENOMEM。

## 可证伪回归与调试结论

- 新同页装载 fixture 在旧实现返回 STATE（`0xb`），新实现两个读者共享同一页且只调用一次 loader。
- 写回 yield 后修改共享别名，旧实现快照字节检查失败；新快照稳定，新 generation 保留至再次同步。再增加等待期间装载同 inode 的另一页，验证该页的缓存 owner 引用不被旧写回误释放。
- files fixture 在文件页 I/O 期间 unmap，禁止发布旧 PTE；嵌套模拟另一个线程先发布同页时，旧重试返回 ADDRESS_SPACE（5），新实现成功。真实 musl pthread 也复现并验证该修复。
- 清洁页回收后的健康 node 不延后到卸载。16 次 open/read/close/reclaim 的卸载前堆 live-allocation 检查在错误延后版本失败（`0x53`），固定 `ext4_fclose` 无 I/O 的契约允许立即释放。
- 最后 close 与已 unlink inode 的写回交错，等待写回结束后再失效缓存，不在 writeback 页上 fatal。
- host recovery 在块写入口检查 mount lock owner。首次在 orphan 回收发现无锁 transaction owner，补齐公开维护入口后通过；fixture 自己直接调用底层 orphan peek 时也须显式持锁，不能把测试裸调用误归因于公开 API。
- 两个文件冷读的 NBD 响应必须同时暂扣；此时计算任务与无关缓存命中必须完成。额外设置 pending termination 并调用 signal wake，两个内部 DMA wait 仍保持 BLOCKED，随后完成、退出和资源基线回收。
- 八个写请求全部暂扣后逆序释放；writeback 模式下一条必须为 FLUSH，再为后继内容回读；write-through 同样排空前八个才放行后继。另一次设备生命周期暂扣八请求直至 guest 一秒 timeout，收到 reset 标记后由 host 放行，验证八个失败结果、单次 reset 及后续提交失败。每项覆盖两种 transport 和两种 cache mode。
- 完整 WAL 验证暴露旧故障启用的信号竞态：probe 为 43 事件，另一次仅 42，末端 cut 永远不会触发。`SIGUSR1` 与客体放行之间缺少后端确认，启用边界会漂移。增加通用 `arm` 控制命令，等待 `control=arm` 后才发客体许可；host 红测先验证旧协议无法在 NBD 空闲时确认，新协议和末端抽样通过后重跑 DELETE/WAL 完整矩阵。
- NBD arm/hold/release/drain 是协议握手，未用固定宿主 sleep 推断完成。QEMU 可把相邻读合并为一个 NBD 请求，因此并发门槛显式关闭 request-merging；普通测试不更改内核合并行为。
- 高半区切换后继续使用编译器保存在寄存器里的物理栈指针曾造成启动 fault。把 DTB 存储探测放入 noinline 调用，避免该指针跨地址切换存活；无盘不创建额外 cleanup task，有盘在资源基线快照前创建。
- 2026-09-29 的 GitHub Actions [run #35](https://github.com/Freefor100/BoarOS/actions/runs/36510992587) 在 legacy 队列收到八个 NBD 写响应后停止前进，另一次离线 GCC 冷启动在块轮询阶段报一秒 timeout。旧驱动先读取 used index，处理完后才写 MMIO ACK；固定 QEMU `references/qemu/hw/virtio/virtio-mmio.c`（commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`）对 ACK 清除 ISR 位，后到的完成可能被清掉而未被收割。固定 Linux `references/linux/drivers/virtio/virtio_mmio.c`（commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`）先 ACK 再调用队列回调。现改为 ACK 后取 used index，并在轮询及期限唤醒时先收割已发布完成再判超时。复核：`make test-io-sleep-riscv test-block-riscv test-block-host test-offline-c-riscv`，以及 RISC-V、规模、userland、679 项差分、glibc、SQLite DELETE/WAL 和栈检查均通过；Ubuntu 24.04/QEMU 8.2.2 容器中 legacy 队列连续 30 次、离线 GCC 冷启动 5 次通过。[后续 run #36](https://github.com/Freefor100/BoarOS/actions/runs/36513240987) 的可睡眠 I/O 门槛通过，但离线 GCC 的启动轮询仍超时，证明两种故障不能合并归因；后者见[离线工具链记录](offline-toolchain-probe.md)。

## 2026-09-28 阶段验收

该阶段生产内核 SHA-256 `5565ddce40a9ade4fac4f7a2191aa5b136b3cc456e92873ab6ef4be3abe9d8cb`；NBD 服务 `356cbb5d10fbe590087eda1f4bc9421f3d56c39bb4f82afd289a97fa4c18bfb1`。RISC-V 全套、真实 static/pthread userland、548 条 Linux 差分、glibc 五种形态、SQLite DELETE/WAL、离线 GCC 五阶段通过。编译器栈检查覆盖 1357 函数，最大 2368 字节（DTB IRQ 解析）；真实 userland 最低剩余栈为 4760/5680 字节。lwIP、record-lock、allocator-release、block、NBD、lwext4 host 与执行器 27＋18 项通过。

规模成本保持：对齐 1 MiB 写入为 256 分块/256 用户页解析；16/64 MiB resident 探测为 6210/24519（3.95 倍）；单页改权各访问 3 级 PTE，地址失效逐页一次、全局失效为零。

本轮确定性队列 fixture 输出的生命周期累计计数如下（提交数含启用 IRQ 前的 fixture 准备，IRQ/唤醒数随合并时机可变化，双值按 legacy/modern 排列，不设墙钟门槛）：

| cache 模式（legacy/modern 均通过） | 提交 | 最大在途 | IRQ | 设备等待 sleep / completion wake | 队列等待 | 运行期轮询 |
|---|---:|---:|---:|---:|---:|---:|
| writeback | 469 | 8 | 4 / 6 | 12 / 12 | 3 | 0 |
| writethrough | 432 | 8 | 10 | 11 / 11 | 16 | 0 |
| 独立超时生命周期 | 8 | 8 | 1 | 8 / 0 | 0 | 0 |

`wakes` 只计驱动完成队列实际唤醒的等待者；超时场景八个任务由 scheduler deadline 到期唤醒，因此 completion wake 为零，八个同步调用仍全部返回 TIMEOUT，且只执行一次失败/reset 仲裁。write-through 的 NBD 后端可在 WRITE 响应之后执行 QEMU 自身的 FLUSH；runner 放行这些持久化响应，再要求 guest 后继读取出现，不能把它们误认为 guest 使用 FLUSH feature。

最终 228 项清单完成：227 pass、1 `busybox.official` upstream-failure，无既有通过项回退。suite identity 为 `257434c6cf372651fdb520d5599198a484ffa8fe005d04e9bf8289cc454aeedd`；BusyBox 仍缺 df/dmesg/free/hwclock。差分/应用 Linux Image 为 `f199c416620394d4ed0f86deb556700b8fc4ebf64261686f25ec2e93e70e4425`，清单使用独立配置 Image `7ca338ec75e681cc68c5d946b3ae633fc0088fd78569b7847528105a9de6c8ec`。

使用上述最终内核与带 `arm` ACK 的 NBD 服务，DELETE/WAL 常规恢复及完整矩阵均退出 0。DELETE 的 148 个事件（101 write、47 flush）覆盖 444 组断电策略、101 个写失败点和 47 个 flush 失败点；WAL 的 42 个事件（26 write、16 flush）覆盖 126 组断电策略、26 个写失败点和 16 个 flush 失败点。每个矩阵场景均恢复两次，每次执行 `e2fsck -fn`；常规入口同时验证 EXTRA/FULL、未提交/已确认提交恢复与固定 Linux 上相同 ELF。此前缺少 ACK 的 WAL 末端超时不计通过。

恢复程序 SHA-256 为 `179be6d2e5ab52c908d4e0547225e7999d9e404ffd05c12f404e9f170fdca5e0`，SQLite 固定归档为 `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d`。并发请求下的 flush 顺序、乱序完成和 reset 由四组合队列 fixture 单独保护，完整恢复矩阵保护持久化结果。

## 验证入口

```sh
make test-io-sleep-riscv test-files-riscv test-files-partial-write-riscv
make test-scale-riscv test-riscv test-userland-riscv test-diff-abi-riscv
make test-glibc-riscv test-sqlite-rollback-riscv test-sqlite-wal-riscv test-offline-c-riscv test-stack-usage
make test-lwip-host test-record-lock-host test-allocator-release-host test-block-host test-nbd-host test-lwext4-host
make test-lwext4-recovery-host
make test-sqlite-recovery-riscv test-sqlite-recovery-matrix-riscv
make test-sqlite-wal-recovery-riscv test-sqlite-wal-recovery-matrix-riscv
python3 -B tests/program-inventory/run.py --reuse-builds --jobs 2 --output build/sleep-inventory
python3 -B tests/prune-build.py
make prune-build
```

PR CI 增加可睡眠存储并发门槛，失败保存 guest/server 日志和磁盘镜像。DELETE/WAL 完整恢复沿用每周一北京时间 02:00 与手动 workflow，固定输入校验不变；glibc 仍严格本地验收。未 push，未运行托管 GitHub Actions；本地执行上述入口验证实现。

## 默认 RT 带宽下的存储进展

`tests/multi-disk-io-riscv.py --rt-load` 是独立的无故障组合模式，不替代原暂扣 B、错误隔离与断电恢复矩阵。用户程序逐轮创建 FIFO、RR 的持续可运行子任务，明确设置默认 950000/1000000 微秒全局预算；子任务在循环中不 yield、不 sleep，普通父任务的计算和双盘 I/O 因而依赖普通类获得预算余量。

每轮 host 在放行父任务前让两台 NBD 进入 hold。检测到每台盘的真实 READ 后立即 drain，不用固定主机 sleep 消耗设备超时；读回内容由 guest 验证。只有普通父任务确认两盘同步写入/读回、host 同时观察两盘成功 WRITE/FLUSH 后，才允许父任务停止 RT 子任务。随后验证 wait 回收、动态盘卸载、根退出的堆/页/任务栈基线；QEMU 退出后独立 dump 两盘检查字节，并运行 `e2fsck -fn`。这证明组合进展和资源回收，不把 QEMU 墙钟作为实时延迟保证，也不宣称两盘请求必定同时在途。

固定 musl 1.2.5 的 `src/sched/sched_setscheduler.c` 包装器直接返回 ENOSYS；出处为 `references/musl/musl-1.2.5.tar.gz`，SHA-256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`。该验收程序明确使用 `syscall(SYS_sched_setscheduler, ...)` 进入内核，避免将 libc 包装器存根误判为内核调度失败。NBD 的普通 event 日志只记录 WRITE/FLUSH，READ 的实际请求由既有 hold/drain 协议证明。

本阶段最终内核 SHA-256 `be5ca22629c904a427241b0f92e9d561d0312952e787ab75870ec4beae0143b3` 与用户 ELF `99db95d4a1c4052fc3fee682922593c45c0d34d7682783eec97f0a98c909e46b` 已完整通过 legacy/modern × writeback/writethrough 四组合；每组合各执行 FIFO、RR 一轮。每次退出均释放 6 个任务栈，最小空余 4360 字节、最大使用 3816 字节，且通过双盘持久字节、fsck 和根页/堆基线检查。可重建入口为 `make test-multi-disk-rt-riscv`；独立指定内核及 ELF 时使用 `python3 -B tests/multi-disk-io-riscv.py --rt-load --kernel <kernel> --program <multi-disk-io-rv>`。这份 RT 组合结果不代替独立存储故障/恢复矩阵的证据。
