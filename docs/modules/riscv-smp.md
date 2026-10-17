# RV 两核执行与服务边界

默认构建和 LA 仍为单 CPU。`kernel-rv-smp` 提供两核调度及显式绑定 MM/files 的内核任务；`USER_SMP` 另开放真实用户两核执行。普通与受控 MM、页缓存、FD/OFD 使用同一保护与引用规则，任务创建时仅检查受控消费者的范围。普通系统分配器的压力挂钩可由远端调用者发布工作，干净回收只尝试取得缓存保护，实际写回继续由既有 CPU0 worker 完成。两种配置中的文件后端、命名空间、设备和网络业务按下述边界串行。

## 真实用户态与CPU0业务交接

`SMP=1 USER_SMP=1` 使用独立 `build/riscv-user-smp/`。用户资格与CPU0服务资格分开：请求affinity按在线逻辑CPU取得、clone/fork继承、exec保留；临时服务状态不改变`getaffinity`。PID1在secondary上线前构造，因此默认集合在首次恢复准备时固定，不能把构造时的CPU0快照当成最终集合。

通用 syscall 分派、匿名缺页及驻留用户复制在发起 CPU 执行，不再按 syscall 编号先尝试驻留 read 或整次迁移。普通文件缓存与用户复制沿完整共同路径运行，只有实际后端工作进入既有缓存 worker。全局 sync 在 VFS 挂载枚举与同步入口进入 CPU0，不能从本地通用分派直接触达 lwext4。命名空间、进程生命周期、设备、网络及尚未并行化的业务在操作入口使用 CPU0 服务范围；嵌套调用只在最外层进入和退出，exec/exit 的执行所有权覆盖真正的提交位置。

`kernel_scheduler_enter_user_service()` 在这些范围迁移实际任务的内核栈和调用现场。资格在 rank40 提交，旧执行 owner 由 `switch_finish` 结束后 CPU0 才可接纳。信号、组退出及停止发布原子 pending 并通知目标 CPU；返回准备才消费通知，业务范围的进入不能提前吞掉它。无通知的本地返回无需 CPU0 交接；新通知到达则重新处理。CPU1 的 syscall 计数记录实际本地分派，不再要求为零。

FIFO/RR用户任务进入CPU0服务时，在来源CPU保留逻辑实时资格；非阻塞syscall不是yield。来源核只允许该owner恢复或更高优先级用户抢占。真正阻塞、yield、RR片尾、降为OTHER、移除来源affinity或退出会结束资格；多个被抢占者可同时保留，按原优先级/FIFO组织。CPU0 的特殊资格索引仅接纳实时服务，避免本核高优先级用户计算饿死异核实时服务；普通服务与缓存 worker 按 OTHER 队列轮转，不能因反复查询而饿死其正在等待的 I/O。服务完成后重新检查用户资格，不把首次准备完的子任务直接送到U-mode。两种节点均借用既有执行/runnable owner，回收前必须摘尽；不固定MM/files，也不引入worker。

2026-10-14 的 RV 整数 ABI 尺寸探针中，实时资格修复使USER_SMP任务由2816增至2896字节，仍由一张4KiB页容纳；每CPU记录由2048增至3664字节，服务索引另为1616字节。每次调度选择O(1)，入/摘维护两CPU及服务索引，现有优先级桶最坏检查仍最多99级，不扫描等待任务。默认及受控SMP构建没有这些新增字段。

服务内 wait finish 用真实在途借用及两线程握手核对归还；显式 yield 在调用 CPU 走统一调度入口。普通任务因服务或 affinity 迁移时排到目标核队尾，不能反复插到已就绪的后端、checkpoint 或缓存 worker 前面；实时抢占、节流和来源资格保留原有顺序。服务期间耗尽 RR 片的状态在恢复用户资格时消费，不把非阻塞业务当成 yield。

所有 MM 共用 operation gate 与 MM raw 发布协议，不保留 `remote_user` 或并发模式字段。匿名、COW 和 ELF 缺页在发起 CPU 准备和提交；文件缺页借用真实来源，冷页的后端读取交 CPU0 worker，恢复后重查 VMA/PTE。撤映射、降权、fork、截断和缓存撤写均在失效确认后归还退休页。驻留匿名复制保持四次实际锁获取，文件来源不使无关匿名 pin 增加 gate。

```sh
make kernel-rv-user-smp
make test-user-smp-host
make test-user-smp-riscv INIT_CONFIG=config/init.json
make test-user-smp-runtime-riscv INIT_CONFIG=config/init.json
make test-user-smp-programs-riscv
make test-user-smp-composition-host test-user-smp-composition-riscv
make run-user-smp-riscv
```

2026-10-14的同ELF原生验证使用`tests/smp/linux-user.config`，固定Linux commit不变但单独启用SMP并生成配置哈希/缓存。MTTCG两核及一核退化、512MiB/1GiB完成独立进程无yield计算、pthread/TLS/全部F/D与FCSR、嵌套信号、远端撤权限/文件EOF、pipe取消、非leader exec、组停止/继续、CPU0/CPU1 FIFO服务/yield/阻塞与RR片尾，以及CPU0高优先级用户下的异核服务前进；runner核对完整内容流、真实U-mode hart采样、当时 CPU0 的通用 syscall 分派、PID1/堆/根盘退出和磁盘检查。host另保护通知消费后的新发布、旧栈owner、晚加入活动MM、COW失败回滚与64GiB稀疏撤映射。静态/动态musl原生公共案例（动态形态另含dlopen TLS）、glibc五形态及1400条原ABI完成SMP Linux同ELF对照，覆盖2/1CPU和双RAM；原static/pthread musl集成consumer在BoarOS同矩阵复核；这部分不称Linux差分。原单CPU策略用例显式固定CPU0，离线affinity位按真实拓扑选取。Ubuntu24.04/QEMU8.2.2本地容器完成原生同矩阵，实际固件/工具身份由runner保存；这不是托管CI运行结果。默认RV/LA core/runtime/platform和原受控SMP组另行回归。

### 用户态原程序与压力组合

`tests/program-inventory/run.py --execution-profile rv-user-smp`仍使用固定229项原程序及原ELF、参数和环境，但Linux参考单独启用SMP。2026-10-14最终内核下，512MiB/1GiB与2/1 CPU四个profile各229项全部通过；本地严格运行使用`--require-pass`，没有reference-not-pass、超时或未到达项。`native-extended.yml`配置相同四格的独立定时job，托管job尚未运行不能由本地结果代替。

`test-user-smp-composition-riscv`把共享匿名/文件映射回收、pipe/futex等待与取消/组退出、SQLite DELETE/WAL及独立重启放进Linux/BoarOS组合。MM ELF 有 21 个有序功能案例，包括匿名和文件共享页的并发首fault，以及普通用户驻留读、冷缓存、用户fault、精确EFAULT前缀、共享位置与seek、close复用和信号、fork排写pin；顺序整页读另检查可选预读及seek取消。Linux和BoarOS功能profile使用同一ELF。BoarOS功能profile启用只读观察钩子但不下达压力/OOM/RFENCE命令，受控profile才运行CPU0真实页账本、目标操作OOM、退休同步暂停和等待登记控制。加压前准备未映射的干净observer文件页，回收统计覆盖账本分配和显式回收，并等待后台轮次发布实际回收数；不要求同步调用抢在后台worker之前回收候选。普通 I/O 控制器在真实 ecall 的复制、来源 pin、匿名 fault 与后端批次处观察 CPU。十种读场景共复制 65,666 字节，双核全部留在发起 CPU1，单核全部在 CPU0；其中冷 64 KiB 请求仅产生一个后端批次，后端必须在 CPU0。三种普通写场景的四次来源 pin 同样留在发起 CPU。共享 offset、竞争 seek、close 复用、fork 排空写 pin、精确 EFAULT 前缀和最终 fsync 继续核对。无观察钩子的正式内核运行九项普通 I/O 场景，测试控制器与 link wrap 不进入发布构建。

同一共享匿名页的双线程首缺页被加入确定性交错：双CPU时两个真实用户任务固定到不同逻辑CPU写同一未驻留页，首个 resolver 在候选提交前等待另一实际任务，直到第二个resolver已提交可写PTE后再继续；单CPU退化时两个任务同绑CPU0。旧路径此时返回`ADDRESS_SPACE`并fatal；固定Linux `mm/memory.c`的`fix_spurious_fault()`（commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`）也将并发页表更新后的fault作为spurious fault处理。现在在所有 MM 的VMA权限仍允许且当前PTE已满足请求时成功返回，不重复发送页级RFENCE：PTE提交者完成其同步，迟到fault返回用户态前的MM激活完成本地刷新；VMA拒绝或PTE权限不足仍走原拒绝/COW流程。测试要求两次真实fault、一次stale retry、两次成功及一次页级同步。该行为见[MM硬件缺页解析](kernel-mm.md#硬件用户缺页解析)。

文件fault另覆盖`resumed=1`重试：第一个读fault取得缓存页引用后停在PTE提交前，第二个CPU上的reader映射同一`MAP_SHARED`页并完成同步；第一个继续时看到已有PTE，真实进入一次内部retry，再次解析同一权限。旧实现对该权限已满足的`resumed` PTE仍发第二次远端同步。测试红灯观测`arrivals=2, retries=1, resumed=1, resolved=2, syncs=2`；当前共同路径只保留 publisher 的一次同步，绿灯要求`syncs=1`。握手在inode共享读保护下让第二reader继续，持页引用和调用栈覆盖第一次准备；测试同时核对返回数据和自然退出owner。

CPU0最后一条启动记录在`riscv_timer_start()`开启IRQ前完成，避免逐字console输出被已运行用户的TTY数据打断。私有测试在IRQ已开时暂停timer横幅，直到UART实际接纳用户队列数据，旧顺序会破坏横幅完整性；当前顺序不进入该暂停，完整启动记录先于用户案例。runner仍严格拒绝损坏的用户协议行，不从程序退出0推导输出通过。

2026-10-14本地QEMU11.1.2下最终12案例ELF完成60次组合；同一ELF和固件在Ubuntu24.04/QEMU8.2.2容器也完成60次。四个CPU/RAM profile的匿名、文件竞争都要求分别两次fault、一次stale/retry及一次页级同步；受控MM退出owner闭合。QEMU8.2.2最终矩阵的2 CPU配置`remote-retirement=1`、1 CPU配置为0。两套环境分别记录QEMU、kernel compiler、Linux config、firmware、kernel及ELF身份，不能混成一次对照或据此声称性能提升。容器使用独立`--build-root`，防止不同交叉编译器共用对象缓存。

```sh
python3 -B tests/program-inventory/run.py --arch riscv --execution-profile rv-user-smp \
  --cpus 2 --cpus 1 --memory 512M --memory 1G --firmware /absolute/fw_dynamic.bin \
  --reuse-builds --require-pass --output build/user-smp-programs-run
make test-user-smp-composition-host
make test-user-smp-composition-riscv QEMU_RISCV_FIRMWARE=/absolute/fw_dynamic.bin
```

普通用户的驻留复制复用任务已持有的 MM/files 句柄、实际 OFD 引用与 MM raw pin 协议，不为每次复制建立第二组业务owner。来源退休节点保留原有OFD/ELF引用，放开MM gate并排空登记回调后才执行真实关闭。

COST_DIAGNOSTICS配置才保存CPU1→CPU0交接等待ticks、含阻塞的服务elapsed ticks、按实际执行owner结算的running ns；整数时钟单位与运行耗时不同。普通构建不持有这些服务计时字段，调度资格由实际CPU/current与服务状态维护，不依赖迁移计数。ext4、设备、协议和命名空间仍在各自 CPU0 边界执行；分配器通知和同步干净回收可就地执行。LA 仍为单核。

## 启动与本地状态

`dtb_read_cpu_info()`验证固定容量的CPU快照，保留真实hart ID，忽略disabled CPU和cpu-map拓扑节点；
boot hart固定在逻辑0，其余按ID排序。重复、损坏、缺boot hart或超容量失败时不改输出。
`secondary.S`使用独立16KiB静态栈与镜像mailbox，不清BSS；过渡映射后的高地址入口重绑gp/tp、
stvec/sscratch及最终satp。内核仍通过`tp→任务→CPU`定位，用户TLS契约保持。

`smp.c`探测HSM/IPI/RFENCE，启动与停止预算各5秒。HSM STARTED不作为内核ONLINE证明：
CPU1完成本地timer和IPI握手才发布ONLINE。停止先收完受控任务的存储owner，确认静态idle、
无切换前驱及就绪资格，再请求HSM stop；CPU0禁止抢占直到STOPPED确认。失败保留可能仍被使用的owner。

timer的周期、下一tick与armed状态属于CPU；本地RR和RT记账保持独立。CPU0推进全局tick、
coarse time、load、等待期限和服务轮询。CPU1新增期限通知CPU0重装事件。

## 调度与资源

统一rank40短锁保护全局runnable成员、每CPU资格索引和执行owner。资格节点内嵌任务并借用全局成员；
摘取任务同时摘除所有CPU索引。选择不扫描另一CPU的任务，索引维护受两CPU容量约束。
普通创建接口默认CPU0；`kernel_thread_create_joinable_mask()`只供CPU0创建带join owner的受控任务。
可睡眠的私有准备不取得已发布owner；最终rank40接纳点重查CPU在线状态，失效时锁外回滚且不发布join。
`kernel_thread_set_cpu_mask()`要求受控任务已BLOCKED、off_cpu且没有runnable/pending成员，否则BUSY。
迁移资格在新栈`switch_finish`释放旧执行owner以后取得，不读取远端live trap frame或运行中栈。
最后存储借用可以在任一受控CPU归还；实际collector只在普通CPU0服务上下文执行，
受控CPU0和CPU1均把owner留给退休队列并通知，不通过放宽服务断言执行清理。

IPI软件事件使用release发布、exchange消费；接收先清SSIP。通知在最外层raw释放后进入固件，
不在对象或调度raw锁内调用SBI。idle关闭IRQ复查资格和事件后WFI，保持SSIE有效。

栈窗口仅CPU0修改。两页map/unmap为一次操作，缺表先私有分配，可能睡眠后重新查表；已有表不申请候选。
全部预检后发布叶子，撤销后先摘空表，完成本地SFENCE与在线远端RFENCE，才释放表、栈及槽位。
发布或摘除非叶父项使用全范围失效，不能只指定某个叶VA；已有父表中的叶修改可使用范围RFENCE。
RFENCE错误走fatal并保留owner。单页接口仍是批量接口的包装。
受控任务只能进入已绑定的 MM 和获准 FD 消费者（匿名 pipe 及下述预开普通 OFD），不得进入一般命名来源、socket/epoll/FIFO、设备或协议。分配器与缓存不再要求私有 arena 或关闭压力挂钩；专项测试可以用独立 arena 制造耗尽，普通用户和压力挂钩测试使用实际系统分配器及已挂载缓存。
`kernel_thread_create_joinable_mm()`在最终 ONLINE 复查后才转移 caller MM，失败不发布 join 或身份；缓存 root 不取得额外引用。当前任务借用覆盖执行，最后 MM 释放由 CPU0 可信栈的统一 join/reaper 路径完成。活动 MM 借用归零也是 CPU 停止条件。

## 验证入口与证据边界

原程序清单可显式选择`rv-user-smp`执行profile：
`make test-user-smp-programs-riscv QEMU_RISCV_FIRMWARE=/absolute/firmware.bin`，
或`tests/program-inventory/run.py --execution-profile rv-user-smp --firmware FILE`。
默认覆盖2/1 CPU和512MiB/1GiB，仍使用原229个ID、ELF、运行时和输出契约；
`--cpus`、`--memory`可选择独立profile，`--kernel`指定冻结核。默认单核入口不变。
同ELF对照使用`tests/smp/linux-user.config`的固定Linux SMP，保留原程序需要的
mqueue、PTY和其他参考环境设施；不以单核reference或仅有计数的结果代替验收。
显式固件、输入与实际MTTCG线程随逐案例报告保存；原案例预算保持，QEMU执行最多180秒，
控制进程另留5秒终止/排空时间。短案例不要求CPU1用户采样非零，专项程序独立证明并行。
参考准备失败、输出失败、超时和BoarOS回收状态分开；参考失败不能算共同通过。

```sh
make test-smp-host test-mm-pin-host test-mm-concurrency-host
make test-smp-fpu-setup-riscv
make test-mm-smp-riscv
make test-mm-smp-userland-riscv INIT_CONFIG=config/init.json
make kernel-rv-smp
make test-smp-riscv QEMU_RISCV64=build/data-path-baselines/sync-b3/qemu-system-riscv64 \
  QEMU_RISCV_FIRMWARE=build/data-path-baselines/sync-b3/firmware.bin
make test-smp-userland-riscv QEMU_RISCV64=build/data-path-baselines/sync-b3/qemu-system-riscv64 \
  QEMU_RISCV_FIRMWARE=build/data-path-baselines/sync-b3/firmware.bin INIT_CONFIG=config/init.json
make run-smp-riscv
```

host直接链接生产DTB、调度/策略/runqueue/raw实现，验证稀疏身份、失败原子性、资格FIFO及同时claim的唯一owner。
`tests/smp/riscv.py`使用MTTCG、2/1 CPU及512MiB/1GiB，记录vCPU线程、工具和输入SHA、完整串口及native退出状态。
2026-10-11本地验收覆盖：

| 契约 | 真实客户机证据 |
|---|---|
| 执行与状态 | 无主动yield的两核计算、mask=3双资格与唯一owner、双向阻塞迁移；SP、返回PC、s0–s11、32个F/D寄存器及FCSR |
| 通知与等待 | 全局tick不翻倍、提交后尚未切走的提前wake、CPU1 timer关闭后的WFI边界、远端新增deadline；RW读者批次/writer FIFO、CPU1 FIFO yield与RR timer |
| 退休与栈 | CPU1/受控CPU0/迁移后归还最后借用、私有创建睡眠后遇STOPPED的回滚；延迟RFENCE确认、第二张缺表OOM、旧翻译替换及槽位复用 |
| 私有allocator | 两核4MiB预留arena，buddy/共享引用/所有slab class/大对象/resize、耗尽、内容与独立owner账本；结束后独占audit并归还arena |
| 组合 | CPU0原静态与pthread ELF、CPU1受控计算及私有arena；PID1、页/堆/任务栈、等待与根盘/设备收口 |

同一SMP测试核完成2/1CPU各512MiB/1GiB的自然退出。最终页基线在可信idle栈上核对，
完整审计在CPU1 STOPPED后执行；不是用串口marker替代内部owner断言。
缺RFENCE扩展、start拒绝、实际guard fault、STOPPED确认失败与RFENCE返回错误在独立双RAM启动验证；
这些启动不计自然退出或资源回收成功。默认RV完整回归及512MiB/1GiB/16GiB启动、双侧glibc五形态、
1366ABI、userland/guard/栈和相关SQLite/NBD、设备、网络及终端门禁通过；未重跑完整比赛Harness或全断电矩阵。

默认关闭COST。2026-10-11调度核心产物中，SMP任务为2752字节（单CPU2688），CPU记录1952字节（单CPU272）；
每个任务仍持一个metadata页，没有为资格节点增加引用或页。secondary常驻16KiB栈及独立idle/CPU记录；
同配置生产ELF的BSS比单CPU多25056字节，包含新增记录和链接对齐。
资格挑选O(1)，入队/摘除最多维护两份资格索引。每次启动报告IPI发送/接收、RFENCE请求与整启动耗时，
次数随交错变化，不作为固定协议常数；未以这些正确性运行宣称并行吞吐收益。
`SMP=1 COST_DIAGNOSTICS=1`在构建前拒绝，单CPU成本路径保持。

实际RFENCE执行器及输入SHA由runner记录；使用`-bios default`时只记录默认选择，
精确固件身份需显式传入`QEMU_RISCV_FIRMWARE`。本地封存执行器和CI执行器各自记录，
本地通过不能代替托管CI结果。

本地依据：`references/opensbi` v1.8.1 `74434f255873d74e56cc50aa762d1caf24c099f8`，
`references/linux` `f4cdf7ca9a1fdcca413157df19753f388a5a224e`，
`references/qemu` `84f07211cc5b4fc6a371559bf8a5de4fb068e648`。
本地执行器为封存QEMU11.1.2/OpenSBI1.8.1；QEMU SHA-256
`5713c546693820d225e87795a14e8177343cb7ac4d9b0610a9e41c7203eb023d`，固件SHA-256
`894e2aef99590fc07ec6c60ab00282b8bc5d5d5bb2a1d0c6ada0c52df24274c0`。
CI身份另记。启动交接及非叶失效依据见[学习说明](../learning/smp-handoff.md)。

## 匿名 MM 验证

`build/riscv-mm-smp-test/`保存独立测试构建，默认内核入口保持单CPU。2026-10-12 的 MTTCG 2/1 CPU、512MiB/1GiB验证了：旧 write pin 排空后降权和 fork、自持pin时gate忙返回冲突并继续归还、持pin任务迁移后原栈/token/内容保持、父子同时 COW 写隔离、实际 SUM 缓存写权限撤销与重映射、延迟 RFENCE 时数据/两级空表 owner 不归还；真实 U-mode 微探针验证42→84的新代码、NX及内核地址拒读。微探针不执行普通用户 syscall，不作为用户线程跨核验收。

绑定任务 metadata/stack OOM 保留 caller MM 和空 join；自然退出要求 pin/active/pending 为零、所有任务与等待存储归还，16MiB 私有 arena 的 available 回到 finalize 后4079页并独占 audit，再归还系统 order12 owner。MM RFENCE及FENCE.I错误各用双RAM独立启动，核对新调用链保留 owner 后 fatal；不计自然退出或回收成功。CPU0 原静态/pthread程序与CPU1私有MM工作重叠的组合独立执行。

2026-10-12匿名MM阶段的RV测试产物为MM记录544字节、pin56字节、SMP task2768字节和CPU记录1968字节；当时默认task2688、CPU288，后续磁盘域尺寸见[存储说明](../learning/sleepable-storage.md#受控磁盘映射的io与页写入交接)。记录和任务仍各占一个metadata页，pin与退休批次不动态分配；单页pin增加实际acquire/release及两段raw，COW共享页才复制4KiB。并发gate入口检查当前任务的pin链，成本为O(K)，K仅是该owner尚持片段数，普通逐页复制每次先归还片段。每段退休最多16个owner；页表准备最多两张，竞争可产生 loser 和重试，不能由容量上界宣称应用变快。runner保存整启动OFF时间、实际指令同步和 owner 计数；没有新增匹配性能门槛、并行吞吐或硬件性能结论。

共享匿名配置位于`build/riscv-mm-shared-smp-test/`，复用同一生产MM和MTTCG runner。
2026-10-12双RAM的2/1CPU验证父子驻留/未驻留/PROT_NONE共享叶、同页首次缺页仅一次对象发布、
共享写可见、私有叶仍COW、单侧降权不影响另一MM的硬件写权限、持pin迁移及最后映射撤销。
暂停RFENCE时三份数据页、两份空表及对象/pin owner仍在；确认后仅最后pin持页，再归还arena。
独立shared错误profile须包含共享链路的owner证据，不能借用旧私有MM marker算通过。
CPU0原静态/pthread与CPU1共享匿名后台负载的组合另验，普通用户仍限CPU0。

`make test-memory-object-concurrency-host test-mm-shared-concurrency-host`保护对象配额/引用及
跨MM fault/撤映射/替换/OOM；`make test-mm-shared-smp-riscv test-mm-shared-smp-userland-riscv`
执行客户机闭环。共享对象请求176字节，使用256字节slab；表示尺寸没有新增锁层。页槽增加准备计数/接纳状态，24→32字节，仍为32字节slab；
准备finish增加一次对象锁和hash点查询，所有准备失败时不残留计费，接纳者撤PTE后内容仍保留。
预热get/find不分配或解析/清零数据页；同对象同页首次只分配/清零/计费一次。跨对象额度竞争
可能先各自准备候选，失败者全部归还。对象引用、预算和共享统计新增原子操作，CAS可重试，
hash链和扩容的既有最坏成本仍在。共享fork/写fault不复制数据页，MM表准备/退休仍有固定容量与
竞争重试；未追加匹配时间实验，整启动OFF时间不是并行收益或实板性能证据。


## 私有 tmpfs 映射域

`test-mm-file-smp-riscv` 使用真实私有 tmpfs、控制者OFD和 arena 内的后备预算，在512MiB/1GiB的MTTCG两核与一核退化验证源准备后截断、EOF/重新增长、跨根共享、私有COW、受保护叶、pin迁移及RFENCE后回收。独立RFENCE错误核对来源/登记/数据/表owner保留后fatal，不记自然回收。`test-mm-file-smp-userland-riscv` 让CPU0原静态/pthread程序与CPU1受控任务同时运行；本受控组合把用户固定CPU0，真实用户两核使用USER_SMP；普通OFD接纳另见文件交接章节。接口和关闭协议见[MM](kernel-mm.md#tmpfs-文件映射)。

```sh
make test-mm-file-smp-riscv
make test-mm-file-smp-userland-riscv INIT_CONFIG=config/init.json
```

产物缓存位于 `build/riscv-mm-file-smp-test/`，默认单CPU入口及SMP+COST拒绝规则保持。新登记状态采用inode独占的首次映射准备，不增加任务字段或永久MM引用；每次启动核对arena、页/表、堆、栈、等待、OFD/登记和私有挂载owner。

## 受控 ext4 映射域

`build/riscv-mm-disk-smp-test/`使用真实第二ext4盘、arena缓存和原系统堆的后端metadata，
根盘与私有挂载同时存在。CPU1或受控CPU0的冷缺页由普通CPU0缓存worker执行，
命中直接取得物理引用；控制者保留OFD/挂载/缓存至任务、MM、别名、pin及请求结束。
范围同步、文件读写/截断和后端最终释放仍由普通CPU0任务负责，未开放一般FD业务。

2026-10-13本地MTTCG验证覆盖2/1CPU、512MiB/1GiB各一次新盘和同盘独立重启，
共8次正常启动；检查冷读、跨根共享、私有COW、EOF/增长、protected fork、pin与失效回收。
实际SUM写证明撤W/RFENCE后不能继续使用旧翻译；持写pin阻止快照完成而无关页继续。
既有CPU0 worker先延期忙快照、继续冷读，再实际写入下一代脏数据；后端直接读回及
独立重启读回各自验证，不能由worker计数替代内容。两次RFENCE错误启动核对
页/表/来源/cache/mount owner保留，不记自然退出。原静态/pthread ELF另在双RAM
与CPU1磁盘任务组合，未据此声明用户线程跨核或ext4/块层直接跨核执行。

```sh
make test-page-cache-concurrency-host test-mm-disk-concurrency-host
make test-mm-disk-smp-riscv INIT_CONFIG=config/init.json \
  QEMU_RISCV_FIRMWARE=build/data-path-baselines/sync-b3/firmware.bin
make test-mm-disk-smp-userland-riscv INIT_CONFIG=config/init.json \
  QEMU_RISCV_FIRMWARE=build/data-path-baselines/sync-b3/firmware.bin
```

自然退出要求arena回到4079页、页/表/堆/任务栈/等待/请求/别名及挂载owner归还，
根盘PID1与设备收口另核对；两盘e2fsck必须成功。OFF日志分列setup/业务/清理tick，
缓存请求、peak pages、后台写入/延期和MM准备/退休计数；它们随交错变化，不是并行收益。
存储配置见[VFS/ext4](vfs-ext4.md#当前成本边界)，SMP+COST继续拒绝；实际QEMU/OpenSBI身份由runner保存。
接口、结构成本和竞态背景见[MM](kernel-mm.md#ext4-文件映射)、
[VFS](vfs-ext4.md#后台写回与水位回收)及[存储学习说明](../learning/sleepable-storage.md#受控磁盘映射的io与页写入交接)。

## 共享 FD 表与匿名 pipe

`kernel_thread_create_joinable_files()`在CPU0准备独立MM/files引用，最终任务发布移入这些handle；创建失败完整归还。控制者只持record引用，不额外持端点，因此真实最后关闭能产生EOF/EPIPE。退出在可信栈释放handle；匿名pipe末OFD可在CPU1归还，普通文件后端仍由CPU0完成最后释放。域校验与FD/pipe协议见[文件接口](kernel-files.md)。

```sh
make test-files-concurrency-host test-pipe-concurrency-host
make test-files-smp-riscv INIT_CONFIG=config/init.json
make test-files-smp-userland-riscv INIT_CONFIG=config/init.json
```

产物默认位于`build/riscv-files-smp-test/`；`FILES_SMP_BUILD`可为独立本地/CI构建选择目录，runner使用Make传入路径。2026-10-13本地QEMU11.1.2及Ubuntu24.04/QEMU8.2.2各完成2/1CPU、512MiB/1GiB的新盘与独立重启8次正常启动，并完成双RAM的原静态/pthread组合。两核使用实际MTTCG线程，无icount；记录各自固件/内核/工具身份，不能由本地容器结果推定远程CI已运行。

实际登记后的提前wake、fd复用后旧请求、EOF/信号仲裁、blocked/off_cpu迁移、两种任务创建OOM和pipe2输出跨页fault均核对原请求内容及owner。持续4KiB包由CPU0写入、CPU1读入真实受控磁盘映射，冷缺页由CPU0 worker前进；原用户组合要求init实际执行且仍存活期间有包接纳。宿主另使用真实tmpfs映射作为缺页读写缓冲，覆盖4/16KiB几何。本受控配置的普通用户仍在CPU0；pipe接口不开放socket/epoll/FIFO或一般文件系统业务，普通文件交接见下节。

自然退出同时核对PID1状态、FD/OFD/端点/等待、MM/pin、页/堆/任务栈、私有挂载与根盘设备owner，并运行两盘fsck；不是只检查串口marker。RV产物FD record160字节、pipe504字节、task2784字节，前两者保持256/512字节slab；复用task已有files字段，未新增任务页。扩容锁内复制上限8192字节，pin/close不分配或全表扫描；没有新增数据中转缓冲或worker。OFF工作tick与收尾分别保留，不据此宣称性能提升。

## 普通文件IO的执行位置

普通用户和受控内核任务的 read/pread/readv、write/pwrite/writev 使用同一实现。FD raw 查槽并取得 OFD 引用，offset mutex 覆盖整次共享位置操作，缓存 metadata mutex 保护索引、LRU、脏链和借用状态。缓存不再叠加 CPU0 IRQ 排他。热读无需 CPU0；匿名目标缺页或 COW 在调用 CPU 解决，冷文件数据只把缺失批次交给既有 CPU0 worker，不迁移整个调用。

候选页、entry 和读取工作区在调用 CPU 准备，未初始化数据保持私有，worker 取得 inode 资格后复查并发布 loading，再复用 128 KiB SG 后端读取。完成后用户复制和错误前缀仍由原调用者处理。共享文件来源写入先完成 pin，再关闭目标写资格；完整冷覆盖不读取旧数据或预清整页。

范围写回复用同一执行队列，每个请求一次只推进最多 128 KiB，未完成时接回队尾，让其他 inode 的冷读、关闭和同步准备先行。调用者只提交一个实际请求并等待最终完成，控制工作区跨窗口复用；每批 entry 借用在本批结束时归还，续批按页号重查。单核直接调用在批末设置短 IRQ 安全点，不逐窗口强制 yield。停止 worker 后取消尚未执行的续批，已经接纳的版本和仍脏的数据分别保留原 owner。worker 的退休清理也按每轮最多 16 项推进，然后让出 CPU。

fsync 的 CPU0 prepare 捕获固定 durable 目标并登记调用者栈上的等待请求，随后执行者继续处理队列。原调用者持有 OFD/file 或映射来源引用；journal worker 在 CPU0 处理完成或失败、消费共享 OFD 错误游标，并在原请求的 raw 保护内发布结果，调用者醒来直接返回。数据交接失败不重复记录写回错误，只读及内存后端即时完成；同步不再有完成后的第二次 CPU0 排队。commit/checkpoint 的既有独立推进和设备屏障不变。独立同步不取得 offset 锁，O_SYNC/O_DSYNC 仍借用真实写操作已经持有的位置资格。

SEEK_DATA/HOLE 和最终缓存 OFD 释放仍复用该执行队列。需要共享 offset 或文件写操作门的调用先取得资格，再交给执行者借用，不能让 worker 等待调用者的锁而挡住其冷读。已执行请求及调用者退出等待现场之前，缓存保留实际请求借用。末次关闭不能在当前 pin/锁下完成时，既有清理链保留原 owner，用户返回准备在资格归还后继续清理。

### 受控文件任务

受控任务创建时仍检查支持的实际消费者、MM 与 heap/allocator 关系，但不封存 FD/OFD，不改变保护方式，也不要求通过额外控制引用避免末次释放。测试可保留独立 arena、挂载和确定性交错；普通用户路径使用真实系统分配器和压力挂钩。命名空间、FIFO、socket/epoll、记录锁及设备业务仍在实际入口进入 CPU0 范围。

```sh
make test-file-io-concurrency-host
make test-file-io-smp-riscv QEMU_RISCV_FIRMWARE=build/data-path-baselines/sync-b3/firmware.bin
make test-file-io-smp-userland-riscv INIT_CONFIG=config/init.json \
  QEMU_RISCV_FIRMWARE=build/data-path-baselines/sync-b3/firmware.bin
```

产物默认`build/riscv-file-io-smp-test/`，可用`FILE_IO_SMP_BUILD`隔离另一工具环境。2026-10-13，本地QEMU11.1.2/OpenSBI1.8.1及Ubuntu24.04/QEMU8.2.2/其实际安装固件各通过2/1CPU、512MiB/1GiB的新盘和独立重启8次，以及双RAM的原静态/pthread组合；实际MTTCG线程、退出状态、页/堆/任务栈/等待、私有挂载和两盘fsck由runner核对。本地容器验证不表示远程CI已经运行。

宿主4/16KiB、2/4线程核对共享offset及独立OFD整次追加的内容隔离；强制暂停实际执行者后close/dup复用、取消排队者并停止worker，原请求仍读原OFD，运行者完成前不得join。真实客户机同时验证磁盘映射作为缓冲区的冷缺页、精确短读写内容/哨兵、tmpfs本地I/O和最后控制引用结束后的arena基线。本受控配置的普通用户仍只在CPU0，USER_SMP另行验收。

热读不创建执行请求，用户copy仍在调用hart；冷请求只排入缺失页，不把整个文件操作转交。运行时时间戳由挂载点raw锁保护共享值与dirty/staged链，锁外唤醒和归还节点；读取不为atime单独排队。可选预读的OFD预测用已有短锁，队列由缓存保护；取消只标记，CPU0预读worker在锁外归还节点。

2026-10-16本地QEMU11.1.2分别以RA0、RA2完成受控文件任务的2/1hart、512MiB/1GiB新盘及独立重启。两hart日志要求实际用户copy发生在hart1，热读requests_queued不增加；冷缺页由CPU0加载且不增加整操作执行请求。共享read/readv、短fault与offset、close/reuse、同步和最终owner继续检查。RA2另通过1/4KiB和held DMA检查。该证据来自受控内核文件入口；真实用户 `ecall` 使用前述完整共同路径。
