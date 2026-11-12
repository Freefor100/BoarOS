# 随机材料、可信初始化与输出所有权

固定参考为 `references/linux/` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`：`drivers/char/random.c` 的 `crng_fast_key_erasure`、`getrandom`、`random_poll` 和 `lib/crypto/blake2s.c`。后者 SPDX 为 GPL-2.0 OR MIT，`kernel/blake2s.c` 保留来源与版权并在项目 GPL-2.0 路线下使用；不是隐藏依赖于 references 的构建输入。

为避免输出泄漏下一次 key，每次请求在单 hart 短关中断区用全局 key 生成首块，仅将前 32 字节保存为新 key；调用者收到后 32 字节，再由独立局部旧 key / counter 生成余量。大块生成和用户复制都在临界区外，局部 key、块和密码原语工作数组显式擦除。 `getrandom` 完成就绪、范围和 MM 检查后，在数据生成与用户复制阶段显式开放 IRQ；每 256 字节检查已交付前缀后的信号，错误仍返回真实前缀，结束恢复入口 IRQ 状态。随机池自身保留短临界区，不在 syscall 入口统一开放中断。此为单 hart IRQ 排他契约，不是 SMP 同步。

BLAKE2s 每次混入最多 32 字节输入与旧 32 字节 key，故混种临界区只执行一次有界压缩；重新取全局 key 避免 IRQ 并发更新被过期快照覆盖。`kernel_random_mix(data,size)` 只混入材料；`kernel_random_credit_bits(bits)` 独立按 bit 累计，饱和到 256 bit 后使 ready 单向置位并唤醒等待者。VirtIO RNG 仅对成功完成的响应按每字节 8 bit 计数。`RNDADDENTROPY` 使用 Linux 两个 native int（熵 bit 数与 payload 字节数）及后续 payload 布局；先检查调用者实际 CAP_SYS_ADMIN，缺少时返回 EPERM，负数拒绝，分块复制失败不计任何熵，完整混入后才增加声明的 bit 数。用户普通写入不计熵。DTB seed、时钟、测试 seed 与用户写入都是不计熵材料。可信设备失败不清除已获得的 ready，也不能把未完成或错误 DMA 缓冲计作可信字节。

`kernel_random_available()` 表示材料，不能用于判断安全初始化。`kernel_random_fill()` 即使完全无材料也提供显式不安全流，供 GRND_INSECURE、urandom 和早期兼容路径；普通 getrandom 与 random 先检查 `kernel_random_ready()` / 等待。早期 exec 仅在已有材料时提供布局随机化与 AT_RANDOM，保留旧启动兼容性，不把它声明为启动即安全。

聚焦验证：`python3 tests/host/random_vectors.py` 将 BLAKE2s 与 Python hashlib 独立实现比较，覆盖空、63/64/65、127/128/129 和多块输入；同一命令还通过真实设备操作表验证未就绪 O_NONBLOCK、阻塞信号返回、poll 与 ioctl 熵计数及错误，并测试 fast key erasure、不可信混种不置 ready、31+1 字节可信初始化、ready 单向保持、无材料输出和等待错误。`tests/diff-abi/random.c` 是真实 U-mode 的 flags、长度、fault、节点读写、pread/lseek 与 poll 对照用例；已整合 ABI 清单双侧执行，host 与真实用户态证据分开。

启动混种后擦除 `boot_info.rng_seed` 的临时副本并清零长度，不改变固件拥有的原始 DTB。RV／LA 的固件种子均只作为不计熵材料；可信设备的初始化和停止契约见[随机设备模块](../modules/riscv-virtio-rng.md)。

在途停止测试先由执行器观察真实 EGD 请求及客户机等待标记，再通过 console 放行退出，期间不返回熵；这样检查的是已提交请求的取消与 owner 归还，不让宿主读取请求与客户机退出竞争。
