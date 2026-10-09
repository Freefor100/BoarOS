# VirtIO RNG 与可信初始化

共用入口是`drivers/virtio/rng.c`和`include/kernel/virtio_rng.h`，使用
[VirtIO框架](virtio-framework.md)的transport/split queue。RV薄MMIO/PLIC适配仍为
`arch/riscv/virtio_mmio_rng.c`；LA由`drivers/virtio/pci_rng.c`连接现代PCI，根从
全部端点身份发现RNG，不假定槽位。支持RV legacy/modern MMIO和LA modern PCI；
不包含packed ring或实板熵质量声明。

## 请求与信任

每实例有独立IRQ owner、一个descriptor、DMA区和普通joinable内核worker。
legacy分配两个目标物理页、used对齐到第二页；modern分配一个物理页，RV为4KiB、
LA为16KiB。队列及64字节数据区由实例持有，不与块设备共享请求/等待者。

worker 首次运行即请求 64 字节。IRQ 获取 used index 后执行 DMA acquire barrier，检查仅一个新完成、descriptor ID 为 0、长度不超过 64，然后仅记录结果并唤醒 worker。worker 将成功的非零响应传给 `kernel_random_mix(..., trusted=1)`，短响应在本轮累计到至少 32 字节后休眠 60 秒，再补种。可信累计和 ready 唤醒由随机核心负责；DTB seed 与用户写入不计熵。已 ready 后的设备失败不撤销 ready。

单次请求最多等待 5 秒。零响应、非法完成和超时先 reset，随后等待 60 秒才重新协商和请求，避免零响应忙循环；正常短响应可以立即继续。worker 的等待检查与 IRQ 唤醒由单 hart 关中断区协调，睡眠会让其他任务运行。IRQ 不生成或复制用户随机数据。

## 生命周期与失败 owner

`riscv_virtio_mmio_rng_start()` 依次 reset、分配 DMA、协商队列、登记 IRQ、创建 worker。失败时按实际已获得的资源回滚。停止设置 stopping 并唤醒、join 回收 worker 栈，再确认 status reset 为 0 后注销 IRQ、擦除 DMA 和归还物理页。合法分配器释放失败是 fatal 不变量错误。

reset未确认时不得释放DMA：`stop()`返回`-EIO`，保留transport context、DMA和
IRQ owner，允许显式重试。运行期间同样保留owner，60秒后先重试reset，不能继续
向旧队列提交。启动错误只有清理成功后才可作为可选RNG不可用继续boot；若owner
尚存，根启动明确失败。PCI包装层先消费worker/DMA/IRQ，再消费BAR；半成品也保持
可停止，不能因core context已清空而遗漏剩余PCI function。

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
python3 -B tests/rng-riscv.py --memory 512M --memory 1G
make test-rng-loongarch test-rng-failures-loongarch
```

旧RV阶段host模拟覆盖两种队列的16+16短响应、零长度退避、非法长度/ID、5秒
超时、60秒补种、ready后失败保持、启动分配/IRQ/worker失败、在途stop和回收。
真实QEMU两种传输共八例，覆盖正常/缺失/EGD暂扣进展与在途结束，并核对根基线。
EGD和host固定数据仅验证传输、等待和所有权，不是熵质量证据。固定QEMU拒绝
reset确认的真实硬件故障没有动态重现；下述共用阶段补上拒绝确认的边界模型。

共用核心增加独立transport模型和16KiB模型，确认reset拒绝时worker可join但DMA/
IRQ仍保留，随后确认成功才归还资源；同一实例可再次启动，无旧ring指针。该拒绝是
边界模型，不声称固定QEMU真的拒绝reset。LA的DMA/任务页/栈页OOM及IRQ登记失败
另有真实PCI注入，两种RAM均核对根页/堆/任务栈/BAR基线与未就绪等待的信号中断。

`tests/arch_profiles.py`提供双架构硬件事实，`rng_runner.py`共用案例、EGD握手、
退出/owner分类及输入身份。RV默认仍为两种MMIO、512MiB、BoarOS八例；LA为固定
Linux先行、BoarOS随后、两种RAM、四种模式共16例。同一原探针ELF逐次校验，
stdout、退出、输入身份与EGD请求保留在临时`rng-run.*`，成功后的镜像立即删除。
正常/缺设备/延迟/在途退出的LA16例及RV八例均通过；独立GNU五形态回归通过。

Linux平台缓存为`build/linux-la-platform`，启用内建RNG/net/failover/RTC；旧
`build/linux-la`保留。LA固定Linux在`fdt_setup`扫描seed后才`parse_early_param`，
因此仅传`random.trust_bootloader=off`不能建立未就绪对照。RNG runner在vCPU
启动前核对并将固件DTB的整个rng-seed property改为FDT_NOP，保留DTB布局与其他事实；
GDB身份和操作在结果中记录，两侧同样使用无boot seed输入，不修改内核或libc。
默认Boar启动仍接受DTB作为不计熵材料，不能据本测试改变其正常启动策略。
