# 用户 ELF64 装载模块

本文描述共用 ELF64 解析、不可变来源、缺页 backing 和映像布局。RV 生产路径用 Sv39/4 KiB；LA 的内存与 PCI/ext4 静态 ELF 用 LA64/16 KiB/三级页表，能力范围见[LA 模块](loongarch-boot.md)。ELF 的背景知识见[ELF 用户程序装载学习总结](../learning/elf-loading.md)，exec 事务见[进程映像替换模块](kernel-exec.md)。

## 稳定单元

| 文件 | 职责 |
|---|---|
| `include/kernel/elf64.h`、`kernel/elf64.c` | 有界 little-endian ELF64 header/program-header 解码；支持调用者提供的 program-header cache |
| `include/kernel/elf64_source.h`、`kernel/elf64_source.c` | 拥有 executable OFD 或借用不可变 reader、一次解析结果和规范化 `PT_LOAD` 区间的引用计数来源 |
| `include/kernel/elf_image.h`、`kernel/elf_image.c` | 共用地址布局、ASLR、初始栈/auxv 和 source-backed VMA 构造 |
| `include/arch/elf.h`、`include/arch/mmu.h` | 构建期 machine、HWCAP、trampoline、用户范围及页表操作 |
| `kernel/exec.c`、`kernel/exec_image.c` | `execve` 路径/解释器事务和共用映像绑定 |
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

`kernel_read_source` 只有 `context`、总长度和精确 `read_at`；回调返回零必须表示整个请求已填满。解析器逐字节解码，不把文件映射成内核整块 buffer。成功要求 ELF64、小端、当前版本、标准 header 大小、1..128 个 program header，且每个 `PT_LOAD` 满足文件范围、`p_filesz <= p_memsz`、虚拟范围不溢出及合法的 `p_align`。非空内存段要求文件偏移与VA同余；`p_filesz=p_memsz=0` 的空LOAD不产生映射，不要求这一同余关系，仍保留文件范围和alignment格式检查。底层读取失败是独立的 `IO` 状态，输出保持不变。`open_cached` 在一次表扫描中把 program header 写入调用者存储，source 不再为布局/缺页重复读取 header。

## 不可变 ELF source

`kernel_elf64_source_create()` 接收一个已打开的 regular-file OFD 和当前架构 machine，成功后消费调用者的 OFD owner；失败仍由调用者持有。source 保存 header、program headers、唯一的绝对 `PT_INTERP` 路径和按虚拟页排序、无重叠的 FILE/ZERO/COMPOSITE runs。source 具备 `create/acquire/release` 引用协议；最后一个 release 按 file、解释器字符串、临时边界、run/table allocation 的顺序清理。真实 VFS/I/O 清理失败保持 source/OFD owner；物理页与堆的合法释放完成即返回，分配器不变量错误进入 fatal。

`kernel_elf64_source_create_reader()` 复制 `read_source` 描述符，不消费 backing。调用者必须让字节与 context 不可变且存活到最后一个 source 引用释放；LA fixture 使用内核只读区中的独立 ELF。reader 的完整 FILE 页同样分配并精确复制，不能按零页处理；OFD 仍走 page cache。两种 source 共用解析、runs、VMA、回滚与引用规则。

`kernel_elf64_source_page(source, allocator, offset, ...)` 以 source-relative、页对齐 offset 查找区间并二分定位：

- 完整文件页从 OFD page cache 借用共享物理页，MM 以 COW PTE 发布；
- 文件/BSS 边界页或页内多段贡献分配私有页，先清零，再精确读取文件字节；
- 纯 BSS 页按需分配并保持全零。

run 边界同时包含每个装载段的首个完整页、最后一个完整文件页和文件尾所在页；FILE 判定覆盖整个 run。这样多页 FILE 区间不会把最后一个不完整文件页一起映射为缓存页。该页由 COMPOSITE 私有物化，只读入 `p_filesz` 内的字节，并把其余 BSS 清零。

物理页分配后若解析或读取失败，source 先释放临时页再返回原始错误；合法释放不会产生重试状态。缺页 I/O 在 MM 层转为用户 `SIGBUS`，物理耗尽转为用户资源失败；source/OFD 的真实 VFS/I/O 清理错误不覆盖原始格式或 I/O 结果。

## RISC-V 映像与 ELF 形态

`kernel_elf_image_build()` 是共用入口，`riscv_elf_image_build()` 保留兼容包装。RV 生产路径固定 Sv39/4 KiB，并接受：

- 非 PIE `ET_EXEC`，有或无解释器；
- PIE `ET_DYN`，带 `PT_INTERP`；
- 无解释器的 `ET_DYN`。

主程序和解释器各由 exec 事务持有一个 source；解释器不能递归含 `PT_INTERP`。`PT_DYNAMIC`、`PT_TLS`、GNU RELRO/STACK 等信息保留在 source 中，供动态链接器读取；内核本身当前不执行重定位、加载额外 DSO 或分配 TLS。主文件格式/架构错误返回 `ENOEXEC`；解释器缺失保留路径错误，解释器短header返回 `EIO`，完整header的格式或架构错误返回 `ELIBBAD`。

每个 source 的 `PT_LOAD` run 映射成 `ELF_PRIVATE` VMA，VMA 的 `backing_offset` 是 source-relative 起点，故障策略为 `ELF`。单个同时要求写与执行的装载段按 RWE 映射；共享文件页写入时仍先 COW，重叠装载段仍不得共享同一虚拟字节或形成跨段的写执行页。完整文件页共享 page cache，写入时 COW；复合页和 BSS 私有化。VMA 与 MM 各保留 source 引用：重复映射不会累积历史引用，fork 的子 MM 取得独立引用，最后一个相关 VMA 消失后释放。每次新建可执行页后先做架构地址转换失效并执行取指同步，覆盖先读后取指的路径。

入口必须按 RISC-V IALIGN 2 字节对齐并落在可执行 `PT_LOAD` 的文件字节中，不能落在 BSS 或段间空洞。`AT_PHDR` 只有完整 program-header table 位于某个 `PT_LOAD` 的文件和内存范围内时才给出用户地址；动态/PIE 映像缺少该地址时视为格式错误。

## 地址布局和初始栈

DTB `/chosen/rng-seed` 至少 32 字节时，`kernel/random.c` 混入该材料并以 ChaCha20 产生独立布局值和 16 字节 `AT_RANDOM`；这只是兼容早期 exec 的不安全降级，DTB 材料不构成可信熵、不使 RNG ready。`kernel_random_available()` 仅表示已有材料；`kernel_random_ready()` 才表示累计取得至少 32 字节可信 VirtIO RNG 输入。既无 DTB 材料也无设备输入时仍启动，沿用确定性旧布局并省略 `AT_RANDOM`。可信源就绪后的输出使用同一状态并保留 ready，设备后续失败不撤回初始化状态。RISC-V 布局独立随机化 PIE/解释器 bias、mmap top-down base、8 MiB 栈 reserve 顶端、brk 起点和 vDSO 页，并逐项检查对齐、Sv39 上界和 VMA 冲突；无合法空洞返回 `ENOMEM`。

栈 VMA 保留 8 MiB，底部 guard 页永不映射；exec 使用线程组当前 `RLIMIT_STACK` 软限制约束初始栈和提交页，按 4 KiB 页粒度计算可提交范围，但不永久缩小 VMA，因此后续调高限额仍能扩展栈。初次只提交覆盖序列化参数和最多 64 KiB headroom 的 RW/NX 页，其余页由匿名 demand-zero fault 提交；参数无法放进软限制时返回 `E2BIG` 并保持旧映像。入口栈按 psABI 16 字节对齐，依次包含 `argc`、`argv[]`、`envp[]`、auxv 和字符串。当前 auxv 提供真实 `AT_PAGESZ`、`AT_PHDR/AT_PHENT/AT_PHNUM`、`AT_BASE`、`AT_FLAGS`、`AT_ENTRY`、`AT_HWCAP=IMAFDC`、`AT_CLKTCK`、固定 root UID/GID、`AT_SECURE=0`、可用时的 `AT_RANDOM`、`AT_EXECFN` 和 `AT_NULL`。

映像建立完成后，MM 记录 source-backed VMA、每 MM 的 mmap ceiling、vDSO 地址和从最高 `PT_LOAD` 内存末端得到的 brk 起点。scheduler 在提交点允许入口页尚未驻留，只要求它属于可执行 VMA；第一次取指由专用 ELF fault backing 完成。

## 所有权和验证

source、临时映像和 MM 的所有权按 exec 事务分层：事务持有创建期 source/OFD，MM 持有 VMA 所需 source 引用，提交成功后事务释放临时 owner。任何失败都先保留原 MM；新 MM 的合法页表/堆释放完成即结束，只有 source/OFD 的真实 VFS/I/O 清理错误进入持久 owner，不能返回“已清理”而丢失资源。

```sh
make test-elf64-riscv
make test-elf-rwx-riscv
make test-elf-tail-riscv
make test-demand-page-riscv
make test-exec-riscv
make test-root-init-riscv
make test-riscv
```

真实根启动 fixture 验证 source-backed 静态入口；动态 musl PIE、解释器、额外 DSO、初始 TLS 和线程运行期间的 dlopen TLS 已通过生产入口验证，消费者复用 userland runner。固定 glibc 2.44 的静态/动态/PIE 与 pthread、dlopen TLS、信号子集由 `make test-glibc-riscv` 对照固定 Linux 验证。重定位与 TLS 分配由用户态动态链接器/libc 完成，不是待添加的内核 ELF 算法。更广 glibc 应用与真实开发板 I-cache/熵源仍需单独验证。LoongArch 已复用同一解析、source和映像策略，验证16 KiB/三级页表的整数内存与PCI/ext4静态ELF、LP64S musl TLS/pthread及整数信号。LA的PT_INTERP、原版LP64D musl动态PIE/非PIE和初始/late DSO TLS已双侧验收，标量FPU与信号扩展见LA模块。

`kernel_elf64_source_create_interpreter()` 复用create所有权契约，仅在完整64字节header之前
区分短EOF的Linux EIO；运行期exec与RV/LA根启动均使用同一入口，失败保留OFD owner。

## 双架构 GNU runtime 验收入口

`tests/userland/glibc/run.py --arch riscv|loongarch` 共用五种 ELF 形态、消费者和
marker 顺序检查。RV 保持固定 glibc 2.44；LA 的 `inputs-loongarch.json` 固定
已安装原版 glibc 2.42、GCC15.1.0、工具/完整 sysroot/CRT/libgcc 身份，包含内容、
权限与 symlink 目标。probe 的版本来自 profile，GNU libc 本体不改写。
LA runtime 实际系统搜索目录为 `/usr/lib64`；不能复制 RV 的 `/lib` fixture。
LA root runner 可显式指定预期退出码，Linux supervisor 与 BoarOS 均核对该值。

`make prepare-la-glibc test-glibc-profile-host` 验证输入及身份反例；
`python3 -B tests/userland/glibc/run.py --arch loongarch --only linux` 的静态、动态、
PIE、静态 PIE、pthread PIE 已在固定 Linux 的512MiB/1GiB通过。初始 BoarOS
静态 ELF 在进入main前SIGILL；SIMD接入后推进至SIGSEGV，GDB在原程序
`__libc_start_main_impl`确认空AT_RANDOM读取。完整 `test-glibc-loongarch` 尚未
通过，等待真实PCI RNG，不把 Linux-only 结果计作 LA 内核支持。
