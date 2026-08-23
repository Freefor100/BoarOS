# RISC-V Sv39 分页模块

本文描述当前 Sv39 建表与启用路径的稳定接口。分页原理、模式选择、叶子大小和规模分析见 [内存管理学习总结](../learning/memory-management.md)，启动顺序见 [RISC-V 启动模块](riscv-boot.md)。

## 入口与职责

| 文件 | 当前职责 |
|---|---|
| `include/arch/riscv/sv39.h`、`arch/riscv/sv39.c` | 初始化根页表、建立 2 MiB/4 KiB 映射并切换 `satp` |
| `arch/riscv/linker.ld` | 导出页对齐的 text、rodata、data 边界 |
| `kernel/main.c` | 根据 DTB RAM、ELF 边界和 QEMU UART 建立启动地址空间 |
| `tests/riscv/sv39_cases.c` | 验证 PTE、页表数量、边界和失败语义 |
| `tests/riscv/sv39_fault_main.c` | 启用分页后验证只读页可读但不可写 |

RISC-V 构建固定 4 KiB 基页。页表初始化从物理页分配器取得并清零一个根页；该分配器的所有可用区间都必须位于 Sv39 可编码的 56 位物理地址范围内，否则初始化在取页前返回 `RISCV_SV39_STATUS_INVALID`。

## 映射契约

`riscv_sv39_map_range` 分别接收虚拟地址和物理地址，不假定二者相等。起始地址、物理地址和长度必须按 4 KiB 对齐，长度必须非零；整个虚拟范围必须位于同一个 Sv39 canonical 半区，整个物理范围必须能由 56 位 PPN 表示。权限至少包含 R、W、X 之一，并拒绝规范保留的 W=1、R=0 组合。

建表器按地址递增处理范围：当当前虚拟地址、物理地址和剩余长度都满足 2 MiB 条件时建立 Level 1 叶子，否则建立 Level 0 的 4 KiB 叶子。非叶表项只设置 V；叶子预置 V、A 和请求的 R/W/X，可写叶子同时预置 D。当前不设置 U 或 G。

现有表项不会被覆盖。若范围中途遇到冲突、页耗尽或其他错误，已经建立的叶子和中间表会保留；这是启动期建表器的部分提交语义。调用者必须放弃出错的整张页表且不得激活它。当前接口不提供回滚、拆分大页、覆盖映射或取消映射。

建表期间代码通过恒等地址访问页表物理页，因此所有映射必须在首次启用分页前完成。运行期修改页表、TLB 定点失效和多 hart shootdown 不在当前接口范围内。

## 启动地址空间

`kernel_main` 当前建立以下恒等映射：

- DTB 报告的整段 RAM；text 为 RX，rodata 为 R，其余 RAM 为 RW。
- QEMU `virt` 的 `0x10000000..0x10001000` UART MMIO 为 RW。

页表页来自启动内存布局的可用 RAM，所以会减少物理页分配器的可用页计数；这些页也落在 RAM 恒等映射内。QEMU UART 地址属于平台事实，不在通用 `sv39.c` 中；接入 VisionFive 2 时应由新的平台入口提供自己的 MMIO 映射。

`riscv_sv39_activate` 使用 ASID 0，把根页物理页号与 MODE=8 写入 `satp`，在写入前后各执行一次全局 `SFENCE.VMA`，并读回 `satp` 验证硬件接受该模式。当前只支持单 hart 的首次切换。

## 验证

```sh
make test-sv39-riscv
make test-sv39-fault-riscv
make test-riscv
```

聚焦建表测试检查 2 MiB/4 KiB PTE 的精确编码、16 GiB 对齐映射的页表规模、混合叶子、canonical/权限/对齐校验、高物理地址拒绝、冲突以及中途失败后的部分提交状态。权限测试先用显式 `ld` 证明 rodata 页可读，再用显式 `sd` 要求产生 store page fault（`scause=15`，`stval` 等于目标地址）。完整启动测试在 512 MiB 和 1 GiB RAM 下验证 `satp.MODE=8`，并要求物理页计数差等于页表页数。

当前未实现 1 GiB 叶子、高半区、用户映射、页表回收和运行期映射修改。
