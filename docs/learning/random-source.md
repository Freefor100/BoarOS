# 随机材料、可信初始化与输出所有权

固定参考为 `references/linux/` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`：`drivers/char/random.c` 的 `crng_fast_key_erasure`、`getrandom`、`random_poll` 和 `lib/crypto/blake2s.c`。后者 SPDX 为 GPL-2.0 OR MIT，`kernel/blake2s.c` 保留来源与版权并在项目 GPL-2.0 路线下使用；不是隐藏依赖于 references 的构建输入。

旧实现先向调用者复制完整 ChaCha20 块，再把该块前 32 字节作为下一次 key；因此知道一次输出即可预测后续输出。新实现每次请求在单 hart 短关中断区用全局 key 生成首块，仅将前 32 字节保存为新 key；调用者收到后 32 字节，再由独立局部旧 key / counter 生成余量。大块生成和用户复制都在临界区外，局部 key、块和密码原语工作数组显式擦除。此为单 hart IRQ 排他契约，不是 SMP 同步。

BLAKE2s 每次混入最多 32 字节输入与旧 32 字节 key，故混种临界区只执行一次有界压缩；重新取全局 key 避免 IRQ 并发更新被过期快照覆盖。`kernel_random_mix(data,size,trusted)` 的 trusted 只能由成功完成的可信 VirtIO RNG 数据设置，累计 32 字节使 ready 单向置位并唤醒等待者。DTB seed、时钟、测试 seed 与用户写入都是不计熵材料。可信设备失败不清除已获得的 ready，也不能把未完成或错误 DMA 缓冲计作可信字节。

`kernel_random_available()` 表示材料，不能用于判断安全初始化。`kernel_random_fill()` 即使完全无材料也提供显式不安全流，供 GRND_INSECURE、urandom 和早期兼容路径；普通 getrandom 与 random 先检查 `kernel_random_ready()` / 等待。早期 exec 仅在已有材料时提供布局随机化与 AT_RANDOM，保留旧启动兼容性，不把它声明为启动即安全。

聚焦验证：`python3 tests/host/random_vectors.py` 将 BLAKE2s 与 Python hashlib 独立实现比较，覆盖空、63/64/65、127/128/129 和多块输入；同一命令还通过真实设备操作表验证未就绪 O_NONBLOCK、阻塞信号返回、poll 与 ioctl 熵计数及错误，并测试 fast key erasure、不可信混种不置 ready、31+1 字节可信初始化、ready 单向保持、无材料输出和等待错误。`tests/diff-abi/random.c` 是真实 U-mode 的 flags、长度、fault、节点读写、pread/lseek 与 poll 对照用例；已整合 ABI 清单双侧执行，host 与真实用户态证据分开。

2026-09-29 窄差分的 34 条记录全部匹配，扩展后的完整 ABI 为 831 条匹配；`make test-random-host test-virtio-rng-host` 通过。`make test-rng-riscv` 两种传输共 8 次启动覆盖就绪、缺设备、信号取消、延迟响应及在途退出，均核对页/堆/栈回收。此次组合内核 SHA-256 `19854baa6e01ab86e11b80c970746dfb0d1e776a787cd0f4c094b25f691ea216`，随机窄探针 SHA-256 `aca76267b36edc4a6507a051041ed882deea323497e67f4c38266f82300648b3`；构建输入由固定 Linux 与 Harness 元数据约束。测试随机字节本身不参与差分。这是随机接口独立阶段的证据；其后的完整调度集成已完成，最新结果与验证快照边界见[程序清单](user-program-inventory.md)及[恢复矩阵](record-lock-sqlite-recovery.md)。

独立提交检查将暂存源码树导出到 `build/random-stage-src/` 后执行 `make -C build/random-stage-src -j4 all`，再用 `tests/rng-riscv.py --kernel build/random-stage-src/kernel-rv` 和 `tests/diff-abi/harness.py --kernel build/random-stage-src/kernel-rv` 重跑；8 次 RNG 启动与 831 条 ABI 全部通过。该独立内核 SHA-256 为 `c9d65f2fb9bfc96c1e393c61d06a937df242608b7a66e23472e686e72f49fd37`，确认不依赖尚未提交的会话和调度改动。

启动混种后立即擦除 `boot_info.rng_seed` 的临时副本并将长度清零，避免初始化后继续保留无用途种子副本；不改固件拥有的原始 DTB。该收尾在独立会话基线核上重跑 8 次 RNG 生命周期启动通过，内核 SHA-256 `b87834c37520eecd114061dde95b5ba3047e4697ae3f4ae659287370100ee530`。
