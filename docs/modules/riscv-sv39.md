# RISC-V Sv39 分页模块

本文描述当前 Sv39 建表与启用路径的稳定接口。分页原理、模式选择、叶子大小和规模分析见 [内存管理学习总结](../learning/memory-management.md)，启动顺序见 [RISC-V 启动模块](riscv-boot.md)。

## 入口与职责

| 文件 | 当前职责 |
|---|---|
| `include/arch/riscv/direct_map.h`、`arch/riscv/direct_map.c` | 校验并转换 direct-map 中的 PA/VA 范围 |
| `include/arch/riscv/sv39.h`、`arch/riscv/sv39.c` | 初始化根页表、建立 2 MiB/4 KiB 映射并切换 `satp` |
| `arch/riscv/linker.ld`、`include/arch/riscv/memory_layout.h` | 固定高半区 VMA 并导出页对齐的 text、rodata、data 边界 |
| `kernel/main.c` | 根据 DTB RAM、ELF 边界和 QEMU UART 建立启动地址空间与高半区别名 |
| `tests/riscv/sv39_cases.c` | 验证 PTE、页表数量、边界和失败语义 |
| `tests/riscv/sv39_fault_main.c`、`tests/riscv/no_identity.c` | 验证只读页写故障和最终地址空间的低 RAM 访问故障 |

RISC-V 构建固定 4 KiB 基页。页表对象首次使用前必须清零。初始化从物理页分配器取得并清零一个根页；该分配器的所有可用区间都必须位于 Sv39 可编码的 56 位物理地址范围内，否则初始化在取页前返回 `RISCV_SV39_STATUS_INVALID`。

页表生命周期是显式状态机：

```text
UNINITIALIZED --init成功--> BUILDING --activate成功--> ACTIVE
                                  +--建表或激活失败--> FAILED
```

只有 `UNINITIALIZED` 可以初始化，只有 `BUILDING` 可以增加映射或激活。`FAILED` 和 `ACTIVE` 都是当前接口的终止状态：重新初始化、继续映射或再次激活均返回 `RISCV_SV39_STATUS_STATE`。初始化失败保持 `UNINITIALIZED`；纯输入校验失败没有修改 PTE，因此保持 `BUILDING`。激活前的内部校验或 `satp` 读回失败会进入 `FAILED`。

## 映射契约

`riscv_sv39_map_range` 分别接收虚拟地址和物理地址，不假定二者相等。起始地址、物理地址和长度必须按 4 KiB 对齐，长度必须非零；整个虚拟范围必须位于同一个 Sv39 canonical 半区，整个物理范围必须能由 56 位 PPN 表示。权限至少包含 R、W、X 之一，并拒绝规范保留的 W=1、R=0 组合。

RISC-V direct map 使用固定公式 `VA = 0xffffffc000000000 + PA`，窗口大小为
128 GiB，因此只接受端点不超过 `0x2000000000` 的非空物理范围。反向转换只接受
`0xffffffc000000000..0xffffffe000000000` 内的非空范围。两种转换都会检查范围越界
和输出指针，失败时不修改输出。

建表器按地址递增处理范围：当当前虚拟地址、物理地址和剩余长度都满足 2 MiB 条件时建立 Level 1 叶子，否则建立 Level 0 的 4 KiB 叶子。非叶表项只设置 V；叶子预置 V、A 和请求的 R/W/X，可写叶子同时预置 D。当前不设置 U 或 G。

现有表项不会被覆盖。若范围中途遇到冲突、页耗尽或其他建表错误，已经建立的叶子和中间表会保留，页表进入 `FAILED`；这是启动期建表器的部分提交语义。状态机会阻止调用者激活这张不完整页表。当前接口不提供回滚、拆分大页、覆盖映射或取消映射。

建表期间 CPU 仍处于 Bare 状态，代码直接使用页表页的物理地址，因此两张启动页表都必须在首次启用分页前完成。运行期修改页表、TLB 定点失效和多 hart shootdown 不在当前接口范围内。

## 启动地址空间

`kernel_main` 当前建立过渡页表和最终页表：

- 过渡页表把覆盖内核镜像的 2 MiB 对齐包络同时映射到低地址和对应高半区，两者临时使用 RWX；QEMU `virt` 的 `0x10000000..0x10001000` UART MMIO 精确映射为 RW。它只服务第一次 `satp` 切换、低地址返回和高半区跳转。
- 最终页表把 DTB 报告的 RAM 建立为固定偏移 direct alias；text 和 rodata 为 R，其余 RAM 为 RW，所有 direct alias 均不可执行。
- 最终页表把内核镜像映射到 `0xffffffff80000000` 开始的高半区别名；它指向同一组物理页，并保持 text RX、rodata R、data/BSS/启动栈 RW。
- 最终页表单独保留 QEMU UART 的低地址 RW MMIO 映射，但不包含任何低地址 RAM 映射。

过渡页表使用内核镜像内静态预留的 5 个 4 KiB 页，不消费正式物理页分配器。最终页表页来自启动内存布局的可用 RAM，所以会减少分配器的可用页计数，并在最终地址空间中落入 direct map。物理页分配器在第二次 `satp` 切换完成后一次性绑定高地址访问函数，分页后的页内访问由此使用 direct map，不会通过低 text 回调暗中依赖过渡页表。QEMU UART 地址属于平台事实，不在通用 `sv39.c` 中；接入 VisionFive 2 时应由新的平台入口提供自己的 MMIO 映射。

`riscv_sv39_activate` 使用 ASID 0，把根页物理页号与 MODE=8 写入 `satp`，在写入前后各执行一次全局 `SFENCE.VMA`，并读回 `satp` 验证硬件接受该模式。切换前保存旧 `satp`；若读回不一致，则经过同样的 fence 恢复旧值，再把页表标为 `FAILED`。

激活期间，旧地址空间和候选地址空间都必须保持当前代码、栈、trap 入口以及页表对象本身可访问，页表对象还必须可写。当前启动路径先从 Bare 切到过渡页表；激活函数从低地址返回后，汇编路径把 `ra`、`sp`、`gp`、`stvec` 和控制流切换到高半区。随后 C 代码从高地址激活最终页表，第二次切换前后所需对象都由高半区内核映射覆盖，因此最终页表不再需要低 RAM。当前只支持单 hart 的启动切换；若错误页表让当前执行地址在写入 `satp` 后立即不可达，CPU 会先产生同步异常，软件无法依赖后续读回路径恢复。

## 验证

```sh
make test-sv39-riscv
make test-sv39-fault-riscv
make test-high-half-trap-riscv
make test-no-identity-riscv
make test-riscv
```

聚焦建表测试检查完整生命周期转换、2 MiB/4 KiB PTE 的精确编码、16 GiB 对齐映射的页表规模、混合叶子、canonical/物理上界/权限/对齐校验、两种叶子结构冲突以及中途失败后的部分提交状态。direct-map 聚焦用例检查正反转换、窗口首尾、跨界和失败时输出不变。独立测试 walker 会从实际页表反向解析 PA、叶子大小和权限；页池预先填充非零字节，以同时验证页表清零。权限测试先用显式 `ld` 证明 rodata 页可读，再用显式 `sd` 要求产生 store page fault（`scause=15`，`stval` 等于目标地址），并在激活后验证所有建表操作均被拒绝。完整启动测试在 512 MiB 和 1 GiB RAM 下验证 `satp.MODE=8`，通过 direct map 实际写读、释放和复用物理页，要求物理页计数差等于最终页表页数，并依据 ELF 符号精确检查高半区 PC、SP、GP 和 `stvec`。独立高半区 trap 测试执行真实 breakpoint，核对 `scause=3` 与高地址 `sepc`；no-identity 测试在最终切换后读取低内核物理地址，要求产生 `scause=13` 的 load page fault。

当前未实现 1 GiB 叶子、用户映射、页表回收和运行期映射修改。direct map 只映射 DTB 报告的第一段 RAM，不包含 MMIO，也不放宽内核 text/rodata 的别名权限。
