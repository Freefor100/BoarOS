# 用户 ELF64 装载模块

本文描述当前有界 ELF64 解析器和 RISC-V 静态用户映像装载器的稳定接口。ELF 结构、装载语义和项目选择的背景见 [ELF 用户程序装载学习总结](../learning/elf-loading.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/read_source.h`、`kernel/read_source.c` | 定义有总长度的精确随机读源和内存 adapter |
| `include/kernel/elf64.h`、`kernel/elf64.c` | 从随机读源解码 ELF64 header/program header，并完成与架构无关的边界检查 |
| `include/arch/riscv/user_elf.h`、`arch/riscv/user_elf.c` | 校验 RISC-V 静态 `ET_EXEC` 布局，把 `PT_LOAD` 物化到新 Sv39 用户空间、建立栈并登记静态 VMA |
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
struct riscv_user_elf_request {
    struct kernel_read_source source;
    struct kernel_exec_string executable;
    const struct kernel_exec_string *arguments;
    size_t argument_count;
    const struct kernel_exec_string *environment;
    size_t environment_count;
};

enum riscv_user_elf_status riscv_user_elf_load(
    const struct riscv_user_elf_request *request,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table,
    struct riscv_sv39_user_space *space,
    struct riscv_user_elf_entry *entry);

enum riscv_user_elf_status riscv_user_elf_register_static_vmas(
    const struct riscv_user_elf_request *request,
    struct kernel_mm *mm,
    struct kernel_heap *heap);
```

请求中的 source、可执行文件名、参数数组、环境数组和字符串都只在调用期间借用；source 背后的文件或内存必须保持有效。`length` 不含结尾 NUL，装载器不会在返回后保留这些指针。非零数量必须配有数组，每个字符串必须配有字节区间，声明区间内不能含 NUL。零个参数会规范化为一个空的 `argv[0]`。参数、环境、可执行文件名、指针表、auxv 和 16 字节对齐填充合计不得超过 128 KiB；超限返回 `RISCV_USER_ELF_STATUS_ARGUMENT_TOO_LARGE`，并在遍历数组前先限制数量，避免恶意计数驱动越界读取。

输入映像必须是 RISC-V ELF64 小端 `ET_EXEC`，并且内核页表为 ACTIVE、与分配器一致，输出空间为 EMPTY。当前明确拒绝 `ET_DYN` 以及含 `PT_INTERP`、`PT_DYNAMIC` 或 `PT_TLS` 的映像，不把缺失的动态链接能力伪装成成功。其他不影响静态装载的 program header 可以忽略。

每个非空 `PT_LOAD` 的内存范围必须位于 `[0x1000, RISCV_USER_ELF_VDSO_BASE)`，至少有 R/W/X 之一，拒绝 RISC-V 保留的 W&&!R 和 W+X。VDSO 位于 stack guard 上方的固定 RX 页，装载段不能覆盖它。两个段的实际内存字节范围不能重叠；仅页对齐包络相交时允许共享同一 4 KiB 叶子，最终权限为相关段权限并集，但并集仍必须满足 W^X。入口必须按 RISC-V 压缩指令允许的 2 字节边界对齐，并落在可执行段的文件内容范围内，不能指向 BSS 或只因页权限合并而可执行的字节。

装载器先完成请求大小、映像结构和布局预检，再建立临时用户空间。所有装载页先清零；`PT_LOAD` 文件内容按目标页边界分块，解析出用户 PTE 的物理页后让 source 直接填入对应 direct-map 地址。RX text 无需临时放宽权限，没有完整文件中间副本，BSS 和页内空隙仍保持为零。所有段复制完成后、临时空间交给 MM 和 U-mode 之前执行一次 RISC-V `FENCE.I`，确保先写入的指令字节不会停留在本 hart 的取指缓存中。

用户栈占用 Sv39 低半区顶端预留的 8 MiB 虚拟区间 `[RISCV_USER_ELF_STACK_RESERVE_BASE, RISCV_USER_ELF_STACK_TOP)`，其下方 `[RISCV_USER_ELF_STACK_GUARD_BASE, RISCV_USER_ELF_STACK_RESERVE_BASE)` 永久不映射；guard 紧上方的 `[RISCV_USER_ELF_VDSO_BASE, RISCV_USER_ELF_STACK_GUARD_BASE)` 映射一页固定 RX VDSO。初次提交从 `page_start(sp - 64 KiB)` 到栈顶的 RW/NX 页：既覆盖不超过 128 KiB 的已序列化初始栈，也在 SP 下方保留至少 64 KiB 立即可用空间；预留区其余部分由真实 U-mode load/store page fault 按 4 KiB 建立匿名零页。ELF 段不得进入 VDSO、guard 或栈预留区。

初始 SP 按 RISC-V psABI 保持 16 字节对齐，并按 Linux 入口形态依次放置 `argc`、`argv[]`、NULL、`envp[]`、NULL、auxv 键值对和高地址字符串。当前 auxv 提供 `AT_PAGESZ=4096`、`AT_PHDR`、`AT_PHENT=56`、`AT_PHNUM`、`AT_BASE=0`、`AT_FLAGS=0`、`AT_ENTRY`、指向请求文件名副本的 `AT_EXECFN` 和 `AT_NULL`；只有完整 program header table 位于某个 `PT_LOAD` 文件范围内时 `AT_PHDR` 才给出其用户虚拟地址，否则为 0。没有伪造尚无可靠来源的 `AT_RANDOM`、HWCAP、身份或平台条目。

`riscv_user_elf_load()` 成功后，调用者先把 space 移入一个单 owner 的 `kernel_mm`，再调用 `riscv_user_elf_register_static_vmas()`。后者用仍有效的同一 source 重新读取和校验静态 header/layout，创建 VMA 集合，以 `RESIDENT_REQUIRED` 和已经按页合并的最终权限登记匿名 ELF VMA，并核对每个相应 PTE；随后以 RW/`DEMAND_ZERO` 登记完整栈 reserve，guard 仍不登记，并登记固定 RX VDSO VMA。它最后取所有非空 `PT_LOAD` 的最高 `p_vaddr + p_memsz` 并向上按 4 KiB 对齐，初始化新 MM 的 start/current break，VDSO 起点作为当前 heap 上界。当前根启动与 exec 事务都在关闭可执行 VFS file 前执行这一步。登记或 break 初始化失败时调用者只能清理这个新 MM，不能发布它。

初始 break 来自内存大小 `p_memsz` 而不是文件大小 `p_filesz`，因此已包含 BSS；使用所有 load segment 的最高末端而不是假设 program header 已排序。break 是每个映像的 MM 状态：普通 fork 复制当时的精确值，成功 exec 由新 ELF 重新初始化，旧映像曾经增长的 heap 不会泄漏到新映像。

## 所有权与失败语义

成功时，装载器把完整 LIVE `riscv_sv39_user_space` 移入 `space`，并最后写出入口和 SP；调用者随后用 `riscv_kernel_mm_create()` 把空间移入通用 MM，再把 MM、入口和 SP 交给 `kernel_user_thread_create()`。普通请求、格式、布局、缺页和地址空间失败时，输出保持不变，已分配的临时页会回收。

Sv39 的零页接口在地址空间内部完成叶子分配、清零、映射和所有权登记。若页已分配但访问与立即释放同时失败，空间进入 `CLEANUP` 并记录这张脱离页表树的页。若装载失败后的地址空间销毁仍不能完成，装载器返回 `RISCV_USER_ELF_STATUS_CLEANUP_REQUIRED`，并把 LIVE 或 CLEANUP 空间移给调用者；调用者必须重试 `riscv_sv39_user_space_destroy()`。`riscv_user_elf_load_detailed()` 还单独输出清理前的映像错误，使 exec 可以保留 `ENOEXEC/E2BIG/ENOMEM/EIO`，而不是让后续清理故障覆盖用户可见原因。这条状态同时覆盖正常树的部分回收与尚未挂入树的单页，错误路径不会丢失页所有权。静态 VMA 登记不转移 source 所有权；它失败后，MM 的 VMA-first cleanup 会释放已创建的部分 metadata。装载器不接管输入 ELF、文件名、参数或环境缓冲区。

## 验证与限制

```sh
make test-elf64-riscv
make test-user-elf-cases-riscv
make test-user-elf-riscv
make test-vma-riscv
make test-demand-page-riscv
make test-brk-riscv
make test-exec-riscv
make test-riscv
```

前两项覆盖随机读解析、source I/O 失败、格式与装载错误树，其中装载用例还读取真实用户 PTE 检查 `argc/argv/envp/auxv`、空参数规范化、128 KiB 恰好可接受的边界、8 MiB 预留区、64 KiB 初始余量、永久 guard 和由最高 load 末端计算的初始 break；它还验证静态 ELF VMA 的共享页权限并集、完整栈 reserve 和 guard 孔洞。第三项由 bare-metal 工具链独立链接三个静态 `ET_EXEC`，用 `readelf` 检查 ELF 形态，再通过内存 source 运行；对象检查确认装载器在激活前发出 `FENCE.I`。`test-root-init-riscv` 与 `test-exec-riscv` 则把独立 ELF 写入 ext4，由 VFS source 驱动装载和 VMA 登记；`test-brk-riscv` 还验证连续 exec 会重置 break。

当前支持内存与已打开 VFS 文件的同步随机读；静态 VMA 登记要求 source 在紧随装载后的重新解析期间仍保持稳定，当前只读根与 exec file owner 满足这一条件。用户指针捕获由通用 exec 层完成。只支持 RISC-V 静态 `ET_EXEC` 和 4 KiB 用户页；匿名栈和 `brk` heap 支持 demand-zero，普通文件另有共享页缓存与 private mmap，但 ELF `PT_LOAD` 仍由装载器同步物化，不使用 file-backed demand paging。固定 VDSO 当前只提供 signal return 序列，不是完整 Linux auxv/VDSO 实现。尚无异步 I/O、`ET_DYN`/ASLR、动态解释器、重定位、TLS、完整 Linux auxv 或 LoongArch 物化器。
