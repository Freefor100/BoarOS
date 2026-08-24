# 用户 ELF64 装载模块

本文描述当前有界 ELF64 解析器和 RISC-V 静态用户映像装载器的稳定接口。ELF 结构、装载语义和项目选择的背景见 [ELF 用户程序装载学习总结](../learning/elf-loading.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/elf64.h`、`kernel/elf64.c` | 从完整只读内存缓冲区解码 ELF64 header/program header，并完成与架构无关的边界检查 |
| `include/arch/riscv/user_elf.h`、`arch/riscv/user_elf.c` | 校验 RISC-V 静态 `ET_EXEC` 布局，把 `PT_LOAD` 物化到新 Sv39 用户空间并建立栈 |
| `tests/riscv/elf64_cases.c` | 覆盖有界解析、截断、格式、段范围和整数溢出 |
| `tests/riscv/user_elf_cases.c` | 覆盖架构、权限、地址布局、共享边界页、回滚和失败所有权 |
| `tests/riscv/user_elf_program.S`、`user_elf.ld`、`user_elf_boot.c` | 独立链接并实际运行完整静态 ELF，验证数据、BSS、栈、RX/guard 故障和回收 |

解析接口为：

```c
enum kernel_elf64_status kernel_elf64_open(
    const void *bytes,
    size_t size,
    struct kernel_elf64_image *image);

enum kernel_elf64_status kernel_elf64_read_program_header(
    const struct kernel_elf64_image *image,
    uint16_t index,
    struct kernel_elf64_program_header *header);
```

`kernel_elf64_open()` 只借用调用者缓冲区，不分配、不复制，也不保留超出该缓冲区生命周期的独立数据。它逐字节解码小端整数，不把不可信字节强转成可能未对齐的 C 结构。成功要求 ELF64、小端、当前 ELF version、标准 64/56 字节 header 尺寸、1..128 个 program header，并验证整个表位于缓冲区内。每个 `PT_LOAD` 还必须满足 `p_filesz <= p_memsz`、文件范围完整、虚拟范围不溢出，以及 `p_align` 为 0/1 或二的幂且 `p_vaddr` 与 `p_offset` 同余。

解析器不决定 CPU、ELF 类型、用户虚拟地址或 PTE 权限，因此以后 LoongArch 可以复用字节解析，而不复用 Sv39 物化代码。`kernel_elf64_read_program_header()` 仍会重新检查索引、偏移加法和缓冲区边界；调用者不能通过伪造 `kernel_elf64_image` 绕过有界读取。

## RISC-V 装载契约

```c
enum riscv_user_elf_status riscv_user_elf_load(
    const void *image,
    size_t image_size,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table,
    struct riscv_sv39_user_space *space,
    struct riscv_user_elf_entry *entry);
```

输入必须是 RISC-V ELF64 小端 `ET_EXEC`，并且内核页表为 ACTIVE、与分配器一致，输出空间为 EMPTY。当前明确拒绝 `ET_DYN` 以及含 `PT_INTERP`、`PT_DYNAMIC` 或 `PT_TLS` 的映像，不把缺失的动态链接能力伪装成成功。其他不影响静态装载的 program header 可以忽略。

每个非空 `PT_LOAD` 必须位于 `0x1000..RISCV_USER_ELF_STACK_GUARD_BASE`，至少有 R/W/X 之一，拒绝 RISC-V 保留的 W&&!R 和 W+X。两个段的实际内存字节范围不能重叠；仅页对齐包络相交时允许共享同一 4 KiB 叶子，最终权限为相关段权限并集，但并集仍必须满足 W^X。入口必须按 RISC-V 压缩指令允许的 2 字节边界对齐，并落在可执行段的文件内容范围内，不能指向 BSS 或只因页权限合并而可执行的字节。

装载器先完成整个映像的结构和布局预检，再建立临时用户空间。所有装载页先清零，随后复制 `p_filesz` 字节，因此 BSS 和页内空隙保持为零。栈固定为 Sv39 低半区最高一张零填充 RW/NX 页，初始 SP 等于低半区用户上界 `RISCV_USER_ELF_STACK_TOP` 并满足 16 字节对齐；这个 SP 是合法的一过尾边界，实际压栈会先递减再访问。紧邻栈下方的一页不映射，因而向下越过栈底会先命中 guard，而不会写坏更低地址的 ELF 段。当前没有构造 `argc/argv/envp/auxv`，也没有按需扩栈。

## 所有权与失败语义

成功时，装载器把完整 LIVE `riscv_sv39_user_space` 移入 `space`，并最后写出入口和 SP；调用者随后可将三者交给 `kernel_user_thread_create()`。普通格式、布局、缺页和地址空间失败时，输出保持不变，已分配的临时页会回收。

Sv39 的零页接口在地址空间内部完成叶子分配、清零、映射和所有权登记。若页已分配但访问与立即释放同时失败，空间进入 `CLEANUP` 并记录这张脱离页表树的页。若装载失败后的地址空间销毁仍不能完成，装载器返回 `RISCV_USER_ELF_STATUS_CLEANUP_REQUIRED`，并把 LIVE 或 CLEANUP 空间移给调用者；调用者必须重试 `riscv_sv39_user_space_destroy()`。这条状态同时覆盖正常树的部分回收与尚未挂入树的单页，错误路径不会丢失页所有权。装载器不接管输入 ELF 缓冲区。

## 验证与限制

```sh
make test-elf64-riscv
make test-user-elf-cases-riscv
make test-user-elf-riscv
make test-riscv
```

前两项覆盖纯解析和装载错误树，其中故障注入会让物理页访问与立即释放同时失败，要求返回可重试的 CLEANUP 所有权并在故障解除后复原页数。第三项由 bare-metal 工具链独立链接三个静态 `ET_EXEC`，用 `readelf` 检查 ELF 形态，再把完整文件嵌入测试 kernel。正常映像在 U-mode 核对 `.data`、零填充 BSS 与用户栈后执行 `exit(93)`；另外两个映像分别写 RX text 和栈下方 guard，必须得到地址与原因匹配的 store page fault。runner 最终还要求任务、用户叶子和页表页全部回收。

当前只接受调用者已经完整读入、在装载期间可读的内核缓冲区；没有流式读取、页缓存或用户指针容错。只支持 RISC-V 静态 `ET_EXEC`、4 KiB 用户页和固定单页栈；没有 `ET_DYN`/ASLR、动态解释器、重定位、TLS、Linux 初始栈、VDSO、文件系统来源、共享文件页、按需分页或 LoongArch 物化器。生产启动没有可执行文件来源，所以不会自动创建 ELF 用户任务。
