# 用户 ELF64 装载模块

本文描述当前 RISC-V 生产 ELF 路径：通用 ELF64 字节解析、不可变来源对象、专用缺页 backing 和 Sv39 映像布局。ELF 的背景知识见[ELF 用户程序装载学习总结](../learning/elf-loading.md)，exec 事务见[进程映像替换模块](kernel-exec.md)。

## 稳定单元

| 文件 | 职责 |
|---|---|
| `include/kernel/elf64.h`、`kernel/elf64.c` | 有界 little-endian ELF64 header/program-header 解码；支持调用者提供的 program-header cache |
| `include/kernel/elf64_source.h`、`kernel/elf64_source.c` | 拥有 executable OFD、一次解析结果和规范化 `PT_LOAD` 区间的引用计数来源 |
| `include/arch/riscv/elf_image.h`、`arch/riscv/elf_image.c` | Sv39 地址布局、ASLR、初始栈/auxv 和 source-backed VMA 构造 |
| `kernel/exec.c`、`arch/riscv/exec.c` | `execve` 的路径/解释器事务和架构映像绑定 |
| `tests/riscv/elf64_cases.c` | 解析边界及 RFC 8439 ChaCha20 已知向量 |

解析器入口为：

```c
enum kernel_elf64_status kernel_elf64_open(
    const struct kernel_read_source *source,
    struct kernel_elf64_image *image);

enum kernel_elf64_status kernel_elf64_open_cached(
    const struct kernel_read_source *source,
    struct kernel_elf64_image *image,
    struct kernel_elf64_program_header *program_headers,
    uint16_t program_header_capacity);
```

`kernel_read_source` 只有 `context`、总长度和精确 `read_at`；回调返回零必须表示整个请求已填满。解析器逐字节解码，不把文件映射成内核整块 buffer。成功要求 ELF64、小端、当前版本、标准 header 大小、1..128 个 program header，且每个 `PT_LOAD` 满足文件范围、`p_filesz <= p_memsz`、虚拟范围不溢出和 `p_align` 同余规则。底层读取失败是独立的 `IO` 状态，输出保持不变。`open_cached` 在一次表扫描中把 program header 写入调用者存储，source 不再为布局/缺页重复读取 header。

## 不可变 ELF source

`kernel_elf64_source_create()` 接收一个已打开的 regular-file OFD 和当前架构 machine，成功后消费调用者的 OFD owner；失败仍由调用者持有。source 保存 header、program headers、唯一的绝对 `PT_INTERP` 路径和按虚拟页排序、无重叠的 FILE/ZERO/COMPOSITE runs。source 具备 `create/acquire/release` 引用协议；最后一个 release 按 file、解释器字符串、临时边界、run/table allocation 的顺序清理。真实 VFS/I/O 清理失败保持 source/OFD owner；物理页与堆的合法释放完成即返回，分配器不变量错误进入 fatal。

`kernel_elf64_source_page(source, allocator, offset, ...)` 以 source-relative、页对齐 offset 查找区间并二分定位：

- 完整文件页从 OFD page cache 借用共享物理页，MM 以 COW PTE 发布；
- 文件/BSS 边界页或页内多段贡献分配私有页，先清零，再精确读取文件字节；
- 纯 BSS 页按需分配并保持全零。

物理页分配后若解析或读取失败，source 先释放临时页再返回原始错误；合法释放不会产生重试状态。缺页 I/O 在 MM 层转为用户 `SIGBUS`，物理耗尽转为用户资源失败；source/OFD 的真实 VFS/I/O 清理错误不覆盖原始格式或 I/O 结果。

## RISC-V 映像与 ELF 形态

`riscv_elf_image_build()` 是生产入口。它固定 Sv39/4 KiB，并接受：

- 非 PIE `ET_EXEC`，有或无解释器；
- PIE `ET_DYN`，带 `PT_INTERP`；
- 无解释器的 `ET_DYN`。

主程序和解释器各由 exec 事务持有一个 source；解释器不能递归含 `PT_INTERP`。`PT_DYNAMIC`、`PT_TLS`、GNU RELRO/STACK 等信息保留在 source 中，供动态链接器读取；内核本身当前不执行重定位、加载额外 DSO 或分配 TLS。主文件格式/架构错误返回 `ENOEXEC`；解释器缺失保留路径错误，解释器格式或架构错误返回 `ELIBBAD`。

每个 source 的 `PT_LOAD` run 映射成 `ELF_PRIVATE` VMA，VMA 的 `backing_offset` 是 source-relative 起点，故障策略为 `ELF`。完整文件页共享 page cache，写入时 COW；复合页和 BSS 私有化。VMA 与 MM 各保留 source 引用：重复映射不会累积历史引用，fork 的子 MM 取得独立引用，最后一个相关 VMA 消失后释放。每次新建可执行页后先做地址转换失效并执行必要的 `FENCE.I`，覆盖先读后取指的路径。

入口必须按 RISC-V IALIGN 2 字节对齐并落在可执行 `PT_LOAD` 的文件字节中，不能落在 BSS 或段间空洞。`AT_PHDR` 只有完整 program-header table 位于某个 `PT_LOAD` 的文件和内存范围内时才给出用户地址；动态/PIE 映像缺少该地址时视为格式错误。

## 地址布局和初始栈

DTB `/chosen/rng-seed` 至少 32 字节时，`kernel/random.c` 以 ChaCha20 产生独立布局熵和 16 字节 `AT_RANDOM`；QEMU `virt` 的固定 DTB 输入提供该种子。没有可信种子时仍启动，但使用确定性旧布局并省略 `AT_RANDOM`，不使用时间、地址或常量冒充熵。RISC-V 布局独立随机化 PIE/解释器 bias、mmap top-down base、8 MiB 栈 reserve 顶端、brk 起点和 vDSO 页，并逐项检查对齐、Sv39 上界和 VMA 冲突；无合法空洞返回 `ENOMEM`。

栈 VMA 保留 8 MiB，底部 guard 页永不映射；初次只提交覆盖序列化参数和 64 KiB headroom 的 RW/NX 页，其余页由匿名 demand-zero fault 提交。入口栈按 psABI 16 字节对齐，依次包含 `argc`、`argv[]`、`envp[]`、auxv 和字符串。当前 auxv 提供真实 `AT_PAGESZ`、`AT_PHDR/AT_PHENT/AT_PHNUM`、`AT_BASE`、`AT_FLAGS`、`AT_ENTRY`、`AT_HWCAP=IMAFDC`、`AT_CLKTCK`、固定 root UID/GID、`AT_SECURE=0`、可用时的 `AT_RANDOM`、`AT_EXECFN` 和 `AT_NULL`。

映像建立完成后，MM 记录 source-backed VMA、每 MM 的 mmap ceiling、vDSO 地址和从最高 `PT_LOAD` 内存末端得到的 brk 起点。scheduler 在提交点允许入口页尚未驻留，只要求它属于可执行 VMA；第一次取指由专用 ELF fault backing 完成。

## 所有权和验证

source、临时映像和 MM 的所有权按 exec 事务分层：事务持有创建期 source/OFD，MM 持有 VMA 所需 source 引用，提交成功后事务释放临时 owner。任何失败都先保留原 MM；新 MM 的合法页表/堆释放完成即结束，只有 source/OFD 的真实 VFS/I/O 清理错误进入持久 owner，不能返回“已清理”而丢失资源。

```sh
make test-elf64-riscv
make test-demand-page-riscv
make test-exec-riscv
make test-root-init-riscv
make test-riscv
```

真实根启动 fixture 验证 source-backed 静态入口；动态 musl PIE、解释器、额外 DSO、初始 TLS 和线程运行期间的 dlopen TLS 已通过生产入口验证，消费者复用 userland runner。重定位与 TLS 分配由用户态动态链接器/libc 完成，不是待添加的内核 ELF 算法。glibc 与真实开发板 I-cache/熵源仍需单独验证；musl 成功不代表所有动态运行时已经兼容。LoongArch 后续复用通用 ELF 解析并提供 16 KiB/三级页表映像后端。
