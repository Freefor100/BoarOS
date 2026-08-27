# 用户 ELF64 装载模块

本文描述当前有界 ELF64 解析器和 RISC-V 静态用户映像装载器的稳定接口。ELF 结构、装载语义和项目选择的背景见 [ELF 用户程序装载学习总结](../learning/elf-loading.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/read_source.h`、`kernel/read_source.c` | 定义有总长度的精确随机读源和内存 adapter |
| `include/kernel/elf64.h`、`kernel/elf64.c` | 从随机读源解码 ELF64 header/program header，并完成与架构无关的边界检查 |
| `include/arch/riscv/user_elf.h`、`arch/riscv/user_elf.c` | 校验 RISC-V 静态 `ET_EXEC` 布局，把 `PT_LOAD` 物化到新 Sv39 用户空间并建立栈 |
| `tests/riscv/elf64_cases.c` | 覆盖有界解析、截断、格式、段范围和整数溢出 |
| `tests/riscv/user_elf_cases.c` | 覆盖架构、权限、地址布局、初始栈边界、共享边界页、回滚和失败所有权 |
| `tests/riscv/user_elf_program.S`、`user_elf.ld`、`user_elf_boot.c` | 独立链接并实际运行完整静态 ELF，验证数据、BSS、参数栈、RX/guard 故障和回收 |

解析接口为：

```c
enum kernel_elf64_status kernel_elf64_open(
    const struct kernel_read_source *source,
    struct kernel_elf64_image *image);

enum kernel_elf64_status kernel_elf64_read_program_header(
    const struct kernel_elf64_image *image,
    uint16_t index,
    struct kernel_elf64_program_header *header);
```

`kernel_read_source` 保存 `context`、总长度和 `read_at`；回调返回零必须表示完整填满请求。内存 adapter 和 VFS file adapter 共享这项语义。`kernel_elf64_open()` 借用并复制 source 描述符，不分配，也不把整个文件复制到内核。它分别读取 64 字节 ELF header 与有界的 56 字节 program header，逐字节解码小端整数。成功要求 ELF64、小端、当前 ELF version、标准 header 尺寸、1..128 个 program header，并验证整个表位于 source 范围内。每个 `PT_LOAD` 还必须满足 `p_filesz <= p_memsz`、文件范围完整、虚拟范围不溢出，以及 `p_align` 为 0/1 或二的幂且 `p_vaddr` 与 `p_offset` 同余。底层读取失败返回独立 `IO` 状态且不修改输出。

解析器不决定 CPU、ELF 类型、用户虚拟地址或 PTE 权限，因此以后 LoongArch 可以复用字节解析，而不复用 Sv39 物化代码。`kernel_elf64_read_program_header()` 仍会重新检查索引、偏移加法和缓冲区边界；调用者不能通过伪造 `kernel_elf64_image` 绕过有界读取。

## RISC-V 装载契约

```c
struct riscv_user_elf_string {
    const char *bytes;
    size_t length;
};

struct riscv_user_elf_request {
    struct kernel_read_source source;
    const struct riscv_user_elf_string *arguments;
    size_t argument_count;
    const struct riscv_user_elf_string *environment;
    size_t environment_count;
};

enum riscv_user_elf_status riscv_user_elf_load(
    const struct riscv_user_elf_request *request,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table,
    struct riscv_sv39_user_space *space,
    struct riscv_user_elf_entry *entry);
```

请求中的 source、参数数组、环境数组和字符串都只在调用期间借用；source 背后的文件或内存必须保持有效。`length` 不含结尾 NUL，装载器不会在返回后保留这些指针。非零数量必须配有数组，每个字符串必须配有字节区间，声明区间内不能含 NUL。零个参数会规范化为一个空的 `argv[0]`。参数、环境、指针表、auxv 和 16 字节对齐填充合计不得超过 128 KiB；超限返回 `RISCV_USER_ELF_STATUS_ARGUMENT_TOO_LARGE`，并在遍历数组前先限制数量，避免恶意计数驱动越界读取。

输入映像必须是 RISC-V ELF64 小端 `ET_EXEC`，并且内核页表为 ACTIVE、与分配器一致，输出空间为 EMPTY。当前明确拒绝 `ET_DYN` 以及含 `PT_INTERP`、`PT_DYNAMIC` 或 `PT_TLS` 的映像，不把缺失的动态链接能力伪装成成功。其他不影响静态装载的 program header 可以忽略。

每个非空 `PT_LOAD` 的内存范围必须位于 `[0x1000, RISCV_USER_ELF_STACK_GUARD_BASE)`，至少有 R/W/X 之一，拒绝 RISC-V 保留的 W&&!R 和 W+X。两个段的实际内存字节范围不能重叠；仅页对齐包络相交时允许共享同一 4 KiB 叶子，最终权限为相关段权限并集，但并集仍必须满足 W^X。入口必须按 RISC-V 压缩指令允许的 2 字节边界对齐，并落在可执行段的文件内容范围内，不能指向 BSS 或只因页权限合并而可执行的字节。

装载器先完成请求大小、映像结构和布局预检，再建立临时用户空间。所有装载页先清零；`PT_LOAD` 文件内容按目标页边界分块，解析出用户 PTE 的物理页后让 source 直接填入对应 direct-map 地址。RX text 无需临时放宽权限，没有完整文件中间副本，BSS 和页内空隙仍保持为零。

用户栈占用 Sv39 低半区顶端预留的 8 MiB 虚拟区间 `[RISCV_USER_ELF_STACK_RESERVE_BASE, RISCV_USER_ELF_STACK_TOP)`，其下方 `[RISCV_USER_ELF_STACK_GUARD_BASE, RISCV_USER_ELF_STACK_RESERVE_BASE)` 永久不映射。初次提交从 `page_start(sp - 64 KiB)` 到栈顶的 RW/NX 页：既覆盖不超过 128 KiB 的已序列化初始栈，也在 SP 下方保留至少 64 KiB 立即可用空间；预留区其余部分暂不映射，当前没有自动扩栈。ELF 段不得进入 guard 或栈预留区。

初始 SP 按 RISC-V psABI 保持 16 字节对齐，并按 Linux 入口形态依次放置 `argc`、`argv[]`、NULL、`envp[]`、NULL、auxv 键值对和高地址字符串。当前 auxv 提供 `AT_PAGESZ=4096`、`AT_PHDR`、`AT_PHENT=56`、`AT_PHNUM`、`AT_BASE=0`、`AT_FLAGS=0`、`AT_ENTRY` 和 `AT_NULL`；只有完整 program header table 位于某个 `PT_LOAD` 文件范围内时 `AT_PHDR` 才给出其用户虚拟地址，否则为 0。没有伪造尚无可靠来源的 `AT_RANDOM`、HWCAP、身份或平台条目。

## 所有权与失败语义

成功时，装载器把完整 LIVE `riscv_sv39_user_space` 移入 `space`，并最后写出入口和 SP；调用者随后用 `riscv_kernel_mm_create()` 把空间移入通用 MM，再把 MM、入口和 SP 交给 `kernel_user_thread_create()`。普通请求、格式、布局、缺页和地址空间失败时，输出保持不变，已分配的临时页会回收。

Sv39 的零页接口在地址空间内部完成叶子分配、清零、映射和所有权登记。若页已分配但访问与立即释放同时失败，空间进入 `CLEANUP` 并记录这张脱离页表树的页。若装载失败后的地址空间销毁仍不能完成，装载器返回 `RISCV_USER_ELF_STATUS_CLEANUP_REQUIRED`，并把 LIVE 或 CLEANUP 空间移给调用者；调用者必须重试 `riscv_sv39_user_space_destroy()`。这条状态同时覆盖正常树的部分回收与尚未挂入树的单页，错误路径不会丢失页所有权。装载器不接管输入 ELF、参数或环境缓冲区。

## 验证与限制

```sh
make test-elf64-riscv
make test-user-elf-cases-riscv
make test-user-elf-riscv
make test-riscv
```

前两项覆盖随机读解析、source I/O 失败、格式与装载错误树，其中装载用例还读取真实用户 PTE 检查 `argc/argv/envp/auxv`、空参数规范化、128 KiB 恰好可接受的边界、8 MiB 预留区、64 KiB 初始余量和永久 guard；段读取失败与物理页故障都必须回收临时空间。第三项由 bare-metal 工具链独立链接三个静态 `ET_EXEC`，用 `readelf` 检查 ELF 形态，再通过内存 source 运行。`test-root-init-riscv` 则把独立 ELF 写入 ext4，由 VFS source 驱动同一装载器。

当前支持内存与已打开 VFS 文件的同步随机读，没有页缓存、共享文件页、异步 I/O 或用户指针来源。只支持 RISC-V 静态 `ET_EXEC` 和 4 KiB 用户页；没有 `ET_DYN`/ASLR、动态解释器、重定位、TLS、完整 Linux auxv、VDSO、按需扩栈/分页或 LoongArch 物化器。生产启动会从只读 ext4 `/init` 创建 PID 1，但尚无通用 `execve` syscall。
