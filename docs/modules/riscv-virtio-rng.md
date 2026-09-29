# RISC-V VirtIO MMIO RNG

入口是 `arch/riscv/virtio_mmio_rng.c` 与 `include/arch/riscv/virtio_mmio_rng.h`。生产根启动在 PID 1、页缓存 worker 和 PLIC 就绪后，从 DTB MMIO 范围及 IRQ route 中发现首个可初始化的 device ID 4；不假定总线槽。支持 RV64 单 hart 的 legacy version 1 与 modern version 2 split queue，不包含 PCI、packed ring 或实板熵质量声明。

## 请求与信任

每实例有独立的 PLIC handler、一个 descriptor、DMA 区和普通 joinable 内核 worker。legacy 分配两个连续 4 KiB 页，used ring 对齐到第二页；modern 分配一个 4 KiB 页。队列及 64 字节数据区由该实例持有，禁止与块设备共享等待者或请求。

worker 首次运行即请求 64 字节。IRQ 获取 used index 后执行 DMA acquire barrier，检查仅一个新完成、descriptor ID 为 0、长度不超过 64，然后仅记录结果并唤醒 worker。worker 将成功的非零响应传给 `kernel_random_mix(..., trusted=1)`，短响应在本轮累计到至少 32 字节后休眠 60 秒，再补种。可信累计和 ready 唤醒由随机核心负责；DTB seed 与用户写入不计熵。已 ready 后的设备失败不撤销 ready。

单次请求最多等待 5 秒。零响应、非法完成和超时先 reset，随后等待 60 秒才重新协商和请求，避免零响应忙循环；正常短响应可以立即继续。worker 的等待检查与 IRQ 唤醒由单 hart 关中断区协调，睡眠会让其他任务运行。IRQ 不生成或复制用户随机数据。

## 生命周期与失败 owner

`riscv_virtio_mmio_rng_start()` 依次 reset、分配 DMA、协商队列、登记 IRQ、创建 worker。失败时按实际已获得的资源回滚。停止设置 stopping 并唤醒、join 回收 worker 栈，再确认 status reset 为 0 后注销 IRQ、擦除 DMA 和归还物理页。合法分配器释放失败是 fatal 不变量错误。

reset 未确认时不得释放 DMA：`stop()` 返回 `-EIO`，保留 mmio、DMA 和 IRQ 的实例 owner，允许显式重试。运行期间 reset 失败也保留该 owner，60 秒后先重试 reset，不能继续向旧队列提交。启动错误只有清理成功后才可作为可选 RNG 缺失继续 boot；若 owner 尚存，根启动明确失败。

`struct riscv_root_boot` 持有实例，其基线在 RNG 分配前采样。根结束和失败清理先 stop/join，再检查页及堆基线。joinable RNG 不发布用户 completion；根 cleanup 只识别 USER PID 1，不会把 RNG worker 当成剩余用户任务。没有 RNG 时用户仍可启动，普通 nonblocking getrandom 保持 `EAGAIN`。

## 固定依据与验证

- Linux `references/linux/drivers/char/hw_random/virtio-rng.c`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`：请求缓冲、合法短响应和 reset 后回收。
- QEMU `references/qemu/hw/virtio/virtio-rng.c`、`backends/rng-egd.c`，v11.1.0 / commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`：异步请求与 EGD 字节协议。

```sh
python3 -B tests/host/virtio_rng.py
make all
build/riscv/musl-root/bin/musl-gcc -fno-link-libatomic -static -O2 \
  -Wall -Wextra -Werror tests/userland/rng.c -o build/riscv/tests/user/rng-rv
python3 -B tests/rng-riscv.py
```

host 模拟覆盖两种队列的 16+16 短响应、零长度退避、非法长度/ID、5 秒超时、60 秒补种、ready 后失败保持、启动分配/IRQ/worker 失败、在途 stop 和资源回收。真实 QEMU 对两种传输分别覆盖正常 `/dev/urandom`、无设备、EGD 暂扣响应时计算任务继续及释放后 getrandom 唤醒、EGD 请求在途结束，共 8 次启动；每次检查根页/堆/任务栈回收。EGD 和 host 固定数据仅验证传输、等待和所有权，不是可信熵质量证据。拒绝 reset 确认的真实硬件故障尚无动态验证，保留 owner 分支经过源码审查。
