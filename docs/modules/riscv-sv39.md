# RISC-V Sv39 分页模块

本文描述当前 Sv39 建表与启用路径的稳定接口。分页原理、模式选择、叶子大小和规模分析见 [内存管理学习总结](../learning/memory-management.md)，启动顺序见 [RISC-V 启动模块](riscv-boot.md)。

## 入口与职责

| 文件 | 当前职责 |
|---|---|
| `include/arch/riscv/direct_map.h`、`arch/riscv/direct_map.c` | 校验并转换 direct-map 中的 PA/VA 范围 |
| `include/arch/riscv/sv39.h`、`arch/riscv/sv39.c` | 建立启动页表与运行期用户根表、切换 `satp` 并回收用户树 |
| `include/arch/riscv/elf_image.h`、`arch/riscv/elf_image.c` | 把已校验的 `ET_EXEC`/`ET_DYN`/`PT_INTERP` source 变成 Sv39 布局、专用 ELF fault VMA 和初始栈 |
| `include/kernel/mm.h`、`arch/riscv/mm.c` | 依据活动 MM 的 VMA 策略处理匿名缺页、区间撤销与权限变更 |
| `arch/riscv/linker.ld`、`include/arch/riscv/memory_layout.h` | 固定高半区 VMA 并导出页对齐的 text、rodata、data 边界 |
| `kernel/main.c` | 根据 DTB RAM、ELF 边界和 QEMU UART 建立启动地址空间与高半区别名 |
| `tests/riscv/sv39_cases.c` | 验证 PTE、页表数量、用户空间生命周期、边界和失败语义 |
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

启动建表器按地址递增处理范围：当当前虚拟地址、物理地址和剩余长度都满足 2 MiB 条件时建立 Level 1 叶子，否则建立 Level 0 的 4 KiB 叶子。非叶表项只设置 V；叶子预置 V、A 和请求的 R/W/X，可写叶子同时预置 D。启动页表不设置 U 或 G。

现有表项不会被覆盖。若范围中途遇到冲突、页耗尽或其他建表错误，已经建立的叶子和中间表会保留，页表进入 `FAILED`；这是启动期建表器的部分提交语义。状态机会阻止调用者激活这张不完整页表。启动建表接口不提供回滚、拆分大页、覆盖映射或取消映射；运行期用户树另有供 MM 使用的 4 KiB owned-range 撤销和改权入口。

启动建表期间 CPU 仍处于 Bare 状态，代码直接使用页表页的物理地址，因此两张启动页表都必须在首次启用分页前完成。运行期用户建表则通过已经绑定的物理页访问函数和 direct map 访问页表页。

## 启动地址空间

`kernel_main` 当前建立过渡页表和最终页表：

- 过渡页表把覆盖内核镜像的 2 MiB 对齐包络同时映射到低地址和对应高半区，两者临时使用 RWX；QEMU `virt` 的 `0x10000000..0x10001000` UART MMIO 精确映射为 RW。它只服务第一次 `satp` 切换、低地址返回和高半区跳转。
- 最终页表把 DTB 报告的 RAM 建立为固定偏移 direct alias；text 和 rodata 为 R，其余 RAM 为 RW，所有 direct alias 均不可执行。
- 最终页表把内核镜像映射到 `0xffffffff80000000` 开始的高半区别名；它指向同一组物理页，并保持 text RX、rodata R、data/BSS/启动栈 RW。
- 最终页表不保留低地址映射；QEMU UART 的物理 MMIO `0x10000000` 映射到 direct-map 窗口之后的 supervisor-only 高半区别名 `0xffffffe000000000`。所有用户根借用该高半区根项，因此用户 trap 期间的内核诊断不需要临时切根，U-mode 又因叶子没有 U 位而无法访问设备。

过渡页表使用内核镜像内静态预留的 5 个 4 KiB 页，不消费正式物理页分配器。最终页表页来自启动内存布局的可用 RAM，所以会减少分配器的可用页计数，并在最终地址空间中落入 direct map。第二次 `satp` 切换完成后，启动代码立即把 UART 驱动从过渡低地址切到最终高半区别名，再绑定物理页的 direct-map 访问函数；此后的页内访问和设备诊断都不依赖过渡映射。QEMU UART 物理地址属于平台事实，不在通用 `sv39.c` 中；接入 VisionFive 2 时应由新的平台入口提供自己的 MMIO 映射。

`riscv_sv39_activate` 使用 ASID 0，把根页物理页号与 MODE=8 写入 `satp`，在写入前后各执行一次全局 `SFENCE.VMA`，并读回 `satp` 验证硬件接受该模式。切换前保存旧 `satp`；若读回不一致，则经过同样的 fence 恢复旧值，再把页表标为 `FAILED`。

激活期间，旧地址空间和候选地址空间都必须保持当前代码、栈、trap 入口以及页表对象本身可访问，页表对象还必须可写。当前启动路径先从 Bare 切到过渡页表；激活函数从低地址返回后，汇编路径把 `ra`、`sp`、`gp`、`stvec` 和控制流切换到高半区。随后 C 代码从高地址激活最终页表，第二次切换前后所需对象都由高半区内核映射覆盖，因此最终页表不再需要低 RAM。当前只支持单 hart 的启动切换；若错误页表让当前执行地址在写入 `satp` 后立即不可达，CPU 会先产生同步异常，软件无法依赖后续读回路径恢复。

## 运行期用户地址空间

`riscv_sv39_user_space_init()` 分配一张新根表，清空低半区根项，并复制已经 ACTIVE 的最终内核页表根项 256..511。用户对象只拥有低半区树；借用的高半区表和叶子始终由内核页表拥有，销毁时不会遍历或释放。

`riscv_sv39_user_map_owned_page()` 只接受 `[0x1000, 2^38)` 内按 4 KiB 对齐的低地址、已分配物理页和合法 R/W/X 权限。成功建立带 U/A（可写时还带 D）的 Level 0 叶子并接管物理页；失败仍由调用者持有该页。`riscv_sv39_user_map_zeroed_page()` 则在地址空间内部完成叶表路径准备、叶子分配、清零、映射和所有权登记，调用者不会接触处于“已经分配但尚无所有者”状态的叶子。中间表分配采用局部事务：若创建第二张中间表失败，会撤销本次新建且仍为空的上级表，不破坏更早的映射。

`riscv_sv39_user_map_cow_page()` 接管调用者的一份 order-0 物理页引用，建立去掉 W 且以 RSW bit 8 标记的 present COW 叶子。`riscv_sv39_user_resolve_cow()` 只接受这种软件标记的页：引用数为 1 时原地恢复最终权限，否则分配并复制新页、替换当前 PTE 后释放旧引用。真正只读而未带 COW 标记的叶子不会被写 fault 放宽。空间统计分别记录当前 COW 页、复制次数和原地恢复次数，供正确性及后续成本测量使用。

`riscv_sv39_user_space_populate()` 用物理页访问函数跨页写入已经映射、当前未激活的 LIVE 用户空间。输入范围必须完整位于 `[0x1000, 2^38)`，零长度也不能以一过尾地址冒充有效起点；当前 `satp` 指向目标根时拒绝操作。它检查映射存在但不要求 PTE 带 W，因此 ELF 装载器可以直接初始化最终权限为 RX 的 text，而不建立临时 RWX 映射或切换到用户根执行复制。

用户空间生命周期为：

```text
EMPTY --init成功--> LIVE --move--> MOVED
  |                  |  \
  |                  |   +--destroy（非当前 root）--> DESTROYED
  +--destroy（非当前 root）--------------------------> DESTROYED
```

Sv39 不为物理页释放失败保留 `CLEANUP` owner。`move` 成功转移整棵 LIVE 页表树，
`destroy` 在当前 `satp` 未指向该根时按叶子、Level 0、Level 1、根表的后序顺序释放；
释放调用完成后对象进入 DESTROYED。分配器检测到非法页、引用或元数据时直接 fatal trap。
若仍由该 LIVE 空间拥有的任一页表页无法经 runtime physical-page access 解析，同样属于
direct-map/allocator/owner invariant 破坏并直接 fatal；没有可恢复 producer，也不保留部分树重试状态。
根物理地址可以为 0，因此是否存在正常树由 `table_pages` 判断，不能由 `root_address != 0`
推断。

`riscv_sv39_user_space_satp()` 只为 LIVE 对象生成 `MODE=8, ASID=0, PPN=root`；`riscv_sv39_switch_satp()` 只接受 Bare 或 Sv39 ASID 0，并在根切换前后执行全局 `SFENCE.VMA`。scheduler 在修改 ready/current 状态之前完成切根。运行期 demand-zero、file-private 或 COW fault 成功更新当前 MM 的单页 PTE 后执行 `SFENCE.VMA fault_va, zero`，只失效本 hart 上该 VA 的 ASID 0 翻译；新填充或复制到执行映射的页面还执行 `FENCE.I`。当前没有 ASID 分配或 SMP 远端 shootdown。

`riscv_sv39_user_unmap_owned_range()` 只处理对齐的 4 KiB 用户范围。调用方先证明目标就是本 hart 当前活动 MM；walker 预检覆盖范围中已有的页表分支、表项形态、物理页可访问性和 active/protected 计数，输入或状态错误在写 PTE 前返回。提交时先把 active 和 protected owner 暂时改为 `V=0` 的软件 PTE，再执行一次本地全局 `SFENCE.VMA`，随后立即释放物理页并清零 PTE。函数返回时不保留临时 invalid PTE、页计数或回收接口；释放不变量错误直接 fatal。

运行期不回收变空的 Level 0/Level 1 表，它们保留到 MM 销毁，以避免在 shrink 提交中增加中间表的额外遍历。连续高水位每 2 MiB 至多保留一张 4 KiB Level 0 表，约为虚拟跨度的 0.2%；极稀疏触页时相对实际驻留数据的比例会更高。

`riscv_sv39_user_protect_owned_range()` 原地改变已有 owner 的权限。非零权限重建同一 PPN 的 U-mode leaf；切换到 `PROT_NONE` 时使用 RSW bit 9 建立 `V=0` 的 protected PTE，并用 bit 8 继续区分 COW。protected owner 保留内容及 exclusive/COW 属性并可恢复，不能与 unmap 的瞬时 invalid PTE 混用。恢复执行权限前先执行 `FENCE.I`，任何实际 PTE 变化后执行本地全局 `SFENCE.VMA`。fork 为 active/protected owner 增加物理页引用并在父子页表保持相同 COW 关系；unmap 和 destroy 都释放自己持有的一份引用。

`riscv_sv39_user_space_fork()` 使用“两阶段子构造—父提交”。它先建立完整子树、逐页 acquire 并记录需要把父可写页转成 COW 的位置；任何分配/acquire 失败只销毁子 owner，父页表保持原样。子空间全部成功后，提交阶段不再分配，只修改父 PTE、更新计数并执行全局本地 `SFENCE.VMA`。这保证普通 fork 的失败原子性，同时把页面内容复制推迟到父或子真正写入时。

RISC-V ELF 映像构造器是当前用户映射接口的真实调用方。它先完成格式、范围、段重叠和页级 W^X 预检，再登记 source-backed VMA；完整文件页在首次 fault 时共享 page cache 并以 COW 发布，文件/BSS 边界页和纯 BSS 页按需私有分配、精确填充与清零。RW/NX 用户栈预留 Sv39 低半区顶端 8 MiB，初次只映射覆盖初始参数栈并额外向下留出 64 KiB 的后缀；其余 reserve 在真实 U-mode load/store page fault 时按 4 KiB 分配零页。预留区下方一页永久没有 VMA/PTE，作为边界 guard。

## 验证

```sh
make test-sv39-riscv
make test-sv39-fault-riscv
make test-high-half-trap-riscv
make test-no-identity-riscv
make test-user-riscv
make test-user-fatal-riscv
make test-demand-page-riscv
make test-brk-riscv
make test-mmap-riscv
make test-riscv
```

聚焦建表测试除启动 PTE、规模和失败语义外，还检查用户根高半区借用、U 页权限、零页/COW 映射、fork 父提交失败原子性、复制与末引用原地恢复、owned-range 参数失败不变、实际撤销/释放、跨页离线填充、用户地址半开区间、数值为 0 的合法用户根地址、lookup、move、活动根销毁拒绝、后序回收、OOM 回滚、`satp` 编码和失败输出不变。VMA/mmap 用例覆盖 protected COW 保持、fixed replace 和 unmap 的 PTE—fence—release 顺序；真实 U-mode mmap 还覆盖匿名和 ext4 文件私有映射、EOF/SIGBUS。非法释放由 allocator fatal-path 测试覆盖。

当前未实现 1 GiB 叶子、`MAP_SHARED` 写共享、ASID 分配和 SMP TLB shootdown。按需提交用于匿名栈、`brk` heap、private-anonymous mmap 和只读普通文件的 private mapping，用户映射固定为 4 KiB；运行期改权/撤销/COW 在单 hart 的不可调度区执行，截断撤销支持非当前 MM，尚无跨 hart 同步协议。direct map 只映射 DTB 报告的第一段 RAM，不包含 MMIO，也不放宽内核 text/rodata 的别名权限。
