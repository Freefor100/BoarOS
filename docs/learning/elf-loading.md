# ELF 用户程序装载学习总结

## ELF 文件解决什么问题

ELF 是“文件中的程序描述”，页表是“运行时虚拟地址到物理页的映射”。装载器负责把前者变成后者：先验证文件声明，再为需要驻留内存的区间准备页面，复制文件字节、补零运行时数据，设置权限，最后给出入口 PC 和初始 SP。仅能进入 U-mode 或复制一段机器码，并不等于已经实现 ELF 装载。

ELF header 描述整个文件的基本形态，包括位数、端序、机器架构、文件类型、入口和 program header table。Program header 描述运行程序时要处理的段；section header 主要服务链接、调试和符号工具。内核装载可执行文件的核心依据是 `PT_LOAD` program header，即使最终文件剥离了 section header 也应能装载。

解析来自文件系统或其他不可信来源的 ELF 时，所有偏移、数量和大小都必须看作攻击者控制的整数。检查 `offset + size <= file_size` 时直接做加法会先溢出；稳妥形式是先确认 `offset <= file_size`，再确认 `size <= file_size - offset`。同样要检查 program header 数量上限、表范围、`p_vaddr + p_memsz`、`p_filesz <= p_memsz` 和对齐关系。逐字节解码还能避开未对齐结构访问、宿主 C 布局和端序假设。

## 三个彼此独立的“ELF 形态”维度

常被混在一起的三个维度其实不同：

- `ET_EXEC` 与 `ET_DYN` 描述装载地址是否固定。传统非 PIE 可执行文件通常是 `ET_EXEC`；PIE 和共享对象通常是 `ET_DYN`，装载器需要选择 load bias，并可能处理 ASLR 和重定位。
- 静态与动态描述运行时是否还需要共享库。动态 ELF 通常带 `PT_INTERP`，内核装入指定解释器，由动态链接器继续装入依赖库和执行重定位；静态 ELF 已把所需库链接进文件，不需要解释器。
- “从哪里读入”描述 I/O 形态。完整内存缓冲区、逐段文件读取和 demand paging 都能装载同一种 ELF，但依赖的文件系统、页缓存和 VM 能力不同。

BoarOS 先用“内存缓冲区中的静态 `ET_EXEC`”验证 ELF 到地址空间的转换，随后把来源收敛为有总长度的精确随机读接口：内存和 VFS 文件提供同一种 `read_at`，program header 小块读取，并由不可变 source 缓存一次解析结果。生产映像把 `PT_LOAD` 规范化为按页排序的 FILE、ZERO、COMPOSITE 区间，由专用缺页路径按需从 page cache 或文件生成用户页；这使得解析、文件生命周期和地址空间映射可以分别验证，同时避免整文件常驻和重复解析。

## `PT_LOAD` 的内存语义

一个 `PT_LOAD` 主要给出：

- `p_offset`：段的文件起点；
- `p_vaddr`：段的运行时虚拟地址；
- `p_filesz`：文件中真实存在的字节数；
- `p_memsz`：运行时需要的字节数；
- `p_flags`：R/W/X 权限；
- `p_align`：文件偏移与虚拟地址必须满足的同余对齐。

装载结果相当于把 `[p_vaddr, p_vaddr + p_memsz)` 先清零，再把文件的 `[p_offset, p_offset + p_filesz)` 复制到开头。`p_memsz - p_filesz` 通常承载 `.bss`，必须为零；不能因为它在文件中没有字节就漏掉映射。段首尾可能不按页对齐，所以复制必须同时处理页内偏移和跨页分块。

一个多页 `PT_LOAD` 可以从完整文件页开始、在最后一页的中间结束文件内容。若用首个页面判定整段均可直接映射文件，尾页的 BSS 会读到 ELF 文件中紧随装载段的 section table 或字符串。FILE 与私有复合页的分界必须落在最后一个完整文件页之后；不仅要清零纯 BSS 页，还要清零文件尾页内 `p_filesz` 之后的字节。

ELF 段权限按字节描述，硬件 PTE 权限按页描述。同一页若同时与多个不重叠段相交，只能取覆盖该页所需权限的并集。这个现象常发生在 linker 把相邻段放到同一页时。实际字节范围重叠会让复制次序决定结果，BoarOS 将其视为无效布局；只共享页对齐边界则允许，但权限并集若同时出现 W 和 X 仍拒绝，以保持 W^X。RISC-V PTE 还规定 W=1、R=0 为保留编码，因此可写 ELF 段必须同时可读。

入口不仅要落在 X 页，还应落在可执行 `PT_LOAD` 的文件字节内。否则入口可能指向 BSS、段间空洞，或者只因相邻段的页权限并集而看似可执行。RISC-V 支持压缩指令时 IALIGN 为 16 bit，所以当前入口要求 2 字节对齐；用户栈则按 psABI 保持 16 字节对齐。

## Linux 形态的初始用户栈

最小架构测试只需一张零填充 RW/NX 页和指向页顶的 SP，但 Linux 进程入口把栈本身当作内核到用户启动代码的 ABI。64 位入口从低地址到高地址依次读取 `argc`、`argv[]`、一个 NULL、`envp[]`、一个 NULL、auxiliary vector 键值对以及字符串；auxv 以 `AT_NULL, 0` 结束。字符串放在高地址、指针表放在低地址，可以先计算最终大小和地址，再一次完成页映射与填充。RISC-V psABI 要求入口 SP 为 16 字节对齐。

Auxv 的值必须来自真实机制，而不是为了让 libc 继续运行而伪造。`AT_PAGESZ` 描述用户基页大小；`AT_PHDR/AT_PHENT/AT_PHNUM` 描述内存中的 program header table；`AT_ENTRY` 是主程序入口；`AT_EXECFN` 指向内核保存的 exec 文件名副本；静态主程序的 `AT_BASE` 为 0；`AT_FLAGS` 当前也为 0。`AT_PHDR` 只有在完整 program header table 随某个 `PT_LOAD` 映入用户空间时才能给出地址，否则只能明确表示不可用。`AT_RANDOM` 需要真正生成并放置随机字节，HWCAP、身份和平台字段也需要各自可靠的数据源；能力尚不存在时省略比填假值更安全。

`argv/envp` 进入装载器时适合使用“指针 + 显式长度”，因为这能在分配页之前检查总大小、整数溢出和嵌入 NUL。Linux `execve(const char *path, char *const argv[], char *const envp[])` 给出的却是用户指针树：内核必须先逐项读取指针，再有界复制每个字符串，形成完全由内核拥有的快照，才能销毁旧地址空间。BoarOS 的通用 exec 层完成这次转换，架构装载器只接收带长度的内核区间。空参数列表规范化成一个空 `argv[0]`，让入口始终得到可遍历的参数向量。

参数限制必须针对最终初始栈，而不只是字符串字节。`argc`、两个指针向量、NULL 分隔、auxv、`AT_EXECFN` 和 16 字节对齐都会消耗空间；若只限制字符串，攻击者仍可用大量空字符串扩大指针表。BoarOS 当前把整个序列化栈镜像限制为 128 KiB，因此这个值是项目实现边界，不等于 Linux 完整的 `ARG_MAX` 策略。

虚拟地址预留与物理页提交是两件不同的事。先在用户地址上界下方保留一个较大的连续栈区间，可以阻止 ELF 段占据未来扩栈范围；初次只映射覆盖参数镜像和一段运行余量的后缀，则不会为每个进程立即消耗整个预留区对应的物理页。向下增长的栈应把永久 guard 放在预留区底部以下；若只把 guard 放在当前已提交页下方，未来扩栈时就必须移动 guard 并处理竞态。BoarOS 现以固定 8 MiB 栈 VMA 作为上限，reserve 内的用户 load/store fault 按 4 KiB 提交零页，底部以下 guard 没有 VMA，因而不会被扩展逻辑接受。

## BoarOS 当前选择

通用层只做有界 ELF64 小端字节解析，不引入通用 VM callback 框架。RISC-V 层固定 Sv39/4 KiB，接受非 PIE `ET_EXEC`、PIE/无解释器 `ET_DYN` 以及非递归 `PT_INTERP`；`PT_DYNAMIC`、`PT_TLS` 和 GNU 扩展元数据保留在 source 中，供用户态动态链接器消费。它按页预检 W^X，建立 `ELF_PRIVATE` VMA，并按 FILE/COW、COMPOSITE 或 ZERO 规则在缺页时物化 `PT_LOAD`。入口要求 2 字节对齐并位于可执行文件字节；栈固定预留低半区顶端 8 MiB，初始参数、指针和对齐合计限制为 128 KiB，初次映射再从 SP 向下多留 64 KiB，其余 reserve 由匿名 demand-zero fault 提交，底部以下保持一页永久 guard。这个拆分让未来 LoongArch64 能复用 ELF 字节解析和栈内容规则，但用 16 KiB/三级页表实现自己的地址布局和页面物化。

当前接口借用一个精确 read source 和带长度的文件名/参数/环境区间。生产启动把已打开的 ext4 `/init` 转成不可变 source；用户 `execve` 则先打开路径、捕获用户字符串，再调用同一装载器。source 由 MM/VMA 持有引用，重复映射不累积历史引用，fork 后子 MM 获得自己的引用；最后一个相关 VMA 消失时释放 source。成功产出独立 LIVE 用户地址空间、入口和 SP，调用者把地址空间移入 MM；scheduler 在提交点替换旧 MM。RISC-V image 同时建立独立随机或无任何混种材料时确定性降级的 PIE/解释器、mmap、brk、栈和 vDSO 布局，并在可用种子时把独立字节放入 `AT_RANDOM`。真实 userland runner 已验证动态 musl PIE、解释器、额外 DSO、初始 TLS 和运行中 dlopen TLS；内核仍不执行重定位或分配 TLS，固定 glibc 五形态矩阵与 `getrandom` 已交付；更广动态 libc/DSO 仍需消费者证据。DTB 材料可支持早期非安全布局，但只有可信输入计数达到门槛才使 RNG ready，见[随机源](random-source.md)。

分配页交给页表之前必须先完成访问、清零和所有权登记；若解析或读取失败，立即释放临时页并返回原始错误。物理页和堆释放遵循 fail-stop 契约，非法 owner、引用或 allocator metadata 进入 fatal，不把 allocator 错误扩散成 CLEANUP 状态。只有 source/OFD 的真实 VFS/I/O 清理错误保留持久 owner，且不能覆盖原本应返回给用户的 `ENOEXEC`、`E2BIG` 或 `ENOMEM`。

本地固定版本的 `oscomp-testsuits` 可作为真实用户态输入和回归来源，但不能决定内核实现顺序或语义。静态程序和 userland runner 的动态 musl 输入都经 source-backed 生产入口验收；更广动态 libc/DSO 负载仍按真实消费者缺口安排，不把单一 fixture 当作全部运行时兼容。

## 验证和调试经验

- 解析器用最小合成字节数组逐项破坏 magic、尺寸、偏移、数量和溢出边界，能快速定位纯格式错误；装载器再单独覆盖架构、地址和权限树。
- 合成 ELF 容易把“想制造的非法对齐”仍写成合法同余关系；失败用例也要重新计算 `p_vaddr % p_align` 与 `p_offset % p_align`，不能只看数值不同。
- 最终集成测试应由真实 linker 生成 ELF，并用 `readelf -h/-l/-S` 核对 `ET_EXEC`、机器类型、`PT_LOAD` 权限与 NOBITS BSS。把文件写入真实根文件系统、经 VFS read source 送入生产装载器后实际 `SRET`，才能覆盖磁盘、文件、解析、物化和硬件执行各层。
- 同时准备正常退出、写 RX text 和向下越过栈底的映像，可以证明 `.data`、BSS、栈、系统调用、PTE W^X、guard 方向和用户故障隔离。结束时比较物理空闲页数，覆盖叶子页、页表页和线程页的所有权闭环。
- 初始栈不能只测一个短字符串。至少要读取真实用户页表验证空参数规范化、argv/envp 的 NULL 分隔、auxv 类型查找、跨页字符串、恰好达到总大小上限的成功边界，以及超过上限时输出和页数均不变化。Auxv 的顺序不是消费者应依赖的接口，测试应按类型扫描。
- 用户可调用的 exec 还要让三个独立 ELF 连续替换，才能同时检查成功不返回、相对/绝对路径、`AT_EXECFN`、寄存器清零、身份与非 CLOEXEC fd 保持。只在装载器单元测试中读取新页表，无法覆盖 Trap Frame 和 point of no return。
- 错误测试不能只看状态码，还要检查输出对象未变化、分配器页数复原；非法页/引用/allocator metadata 应验证进入 fatal，真实 VFS/block I/O 失败则验证 owner 留在正确的 mount 或文件对象并在后续清理机会继续处理。多个回收动作连续失败时还要核对最具体的状态码没有被后续失败降格。
- 高半区内核在第一次地址迁移前仍通过物理别名执行。此时，自动聚合初始化若包含链接器符号指针，编译器可能把整个常量放进高半区 rodata，早期代码读取它就会 fault；逐字段运行时赋值通常能生成可在物理别名执行的 PC 相对取址。早期测试夹具也必须服从这个启动边界，不能因为它不是生产代码就假定最终高半区已经可访问。

## 资料依据

- [System V ABI ELF gABI](https://refspecs.linuxfoundation.org/elf/gabi4+/contents.html)：ELF header、program loading、segment permissions 和 program interpreter。
- [RISC-V ELF psABI](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/)：RISC-V ELF、过程调用和栈对齐约定。
- `references/oscomp-testsuits/` 的 `final-2026` 与 `pre-2025` 固定提交：比赛静态程序和动态运行库输入。
- `references/linux/fs/binfmt_elf.c`、`fs/exec.c`：Linux ELF 装载、`PT_INTERP` 和用户初始栈的成熟实现。

## shebang 与 shell 回退（2026-09-28）

固定依据为 `references/linux/fs/binfmt_script.c` 和 `fs/exec.c`，commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`。内核只对 `#!` 进行解释器转换，
没有 shebang 的文本返回 ENOEXEC；BusyBox ash 的文本回退属于 shell。
不能把 libctest 包装脚本失败全部归因于缺少 shebang：官方镜像的
`run-static.sh`/`run-dynamic.sh` 没有 shebang，还需要可用 shell、工作目录和正确运行库。

本轮差分先发现精简 Linux 关闭了 BINFMT_SCRIPT，导致无意义的双侧 ENOEXEC；
开启后旧 BoarOS 在 argv/env、嵌套、缺失解释器与递归四组实际失败。
`tests/diff-abi/exec_script.c` 与 `make test-exec-riscv` 保存重建输入，
不依赖比赛磁盘。exec 准备事务统一持有脚本/解释器 OFD 和重组后的参数；
只有新映像准备成功后才进入提交阶段。

验证入口为 `make test-exec-riscv test-userland-riscv test-glibc-riscv test-diff-abi-riscv test-stack-usage`。
本轮完整差分 583 条，含 15 条脚本记录；对应测试和真实程序回归通过。
交接起点为 child-TID 的 `ed6acfb`，通用交付见 main 历史；评测分支合入后
应使用原 BusyBox 消费脚本能力，不改上游脚本内容。

两类容易漏掉的 Linux 边界都先由同 ELF 差分证伪旧实现：

- 第六个脚本层的格式错误/缺失解释器先返回 ENOEXEC/ENOENT，不能提前 ELOOP。
  深度检查位于下次格式分派前；覆盖五层成功、六层超限及边界错误。
- `#!/init `（无换行）保留一个空可选参数，不能用参数首字节非零代替“参数存在”。
  换行查找遵循 strnchr 的 NUL 终止；覆盖 EOF 和嵌入 NUL。

原盘 BusyBox v1.33.1 的 ash 回退还依赖 `/proc/self/exe`。固定源码
`references/oscomp-testsuits/busybox/shell/ash.c:tryexec`（仓库 commit
`b5ec6ef8497e1818cbdec3b54bb722f036e57972`）在 ENOEXEC 后用
`bb_busybox_exec_path` 重启 shell，并非改用 `/bin/sh`。评测分支实测无 shebang
文本直接执行为 127，显式 `busybox sh 文件` 为 0；内核 ENOEXEC 正确不等于此
用户态回退环境已齐全。原盘身份和重建边界由评测分支模块记录，主线不伪造 proc 内容。

## GNU空LOAD与装载拒绝（2026-10-07）

双架构评测监督器的LA整数ELF由GNU链接器生成两个PT_LOAD，第二段
filesz/memsz均0、offset0xb0、VA0、align0x4000。固定Linux
`references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`
的`fs/binfmt_elf.c::elf_load`对这种段不映射文件页；同一个原ELF实际执行成功，
BoarOS却在通用parser的同余检查返回ENOEXEC。source/run和image层本来就跳过空段，
故修正parser而没有改变监督器或链接输入。仍拒绝filesz大于memsz，以及有BSS
内存但不同余的段；alignment格式和文件范围规则保持。

`make test-elf64-riscv`新增cached/uncached反例先红（failures=1）后绿，
ELF权限及exec/root聚焦通过。冻结的LA ELF SHA-256为
`c609adb22c06ad374e0d3ef85e49a37c70550e5ab4549835bf8f94ed6bfd1b59`，
在原失败fixture上用修正内核执行，512MiB/1GiB均完成退出和根owner基线。
这项是共用装载纠错，不表示原评测全量或容器验收已经完成。
