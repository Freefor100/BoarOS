# RV 两核执行与服务边界

默认构建和LA仍为单CPU。`kernel-rv-smp`独立配置最多两个逻辑CPU，CPU0是固件选择的boot hart，
CPU1运行显式创建的受控内核任务，可绑定匿名及私有arena tmpfs/受控ext4文件映射MM，也可绑定同域共享FD表执行匿名pipe I/O。CPU0保留构造、截断、MM/fork发布和最后后端释放；同域tmpfs普通I/O就地执行，ext4实际I/O交给CPU0 worker；控制者保留独立OFD、挂载和缓存owner，命名空间运行期间不修改。本受控配置的用户任务、现有worker、实际磁盘I/O、未开放外部后备和系统压力回收仍在普通CPU0服务上下文执行。
受控任务已验证fork后跨MM的共享匿名/真实tmpfs/磁盘缓存页与私有COW；普通用户使用下面的独立USER_SMP配置，原受控配置仍把用户固定在CPU0。

## 真实用户态与CPU0业务交接

`SMP=1 USER_SMP=1` 使用独立 `build/riscv-user-smp/`。用户资格与CPU0服务资格分开：请求affinity按在线逻辑CPU取得、clone/fork继承、exec保留；临时服务状态不改变`getaffinity`。PID1在secondary上线前构造，因此默认集合在首次恢复准备时固定，不能把构造时的CPU0快照当成最终集合。

`kernel_scheduler_enter_user_service()`迁移实际任务的内核栈和调用现场。资格在rank40提交，旧执行owner由`switch_finish`结束后，CPU0才可接纳。同步fault及通用syscall业务在分派前交接；普通磁盘文件的`read/pread/readv`先尝试在发起hart完成驻留缓存到驻留用户页的复制，具体边界见[普通文件IO](#普通文件io的执行位置)。CPU1本地timer/IPI只处理同步元数据，无pending时直接恢复用户态。CPU0信号/组退出发布原子pending位并通知目标CPU；消费之后到达的新通知保留，返回前再次检查。晚于检查的IPI由硬件中断接纳，不要求远端读取live frame。

FIFO/RR用户任务进入CPU0服务时，在来源CPU保留逻辑实时资格；非阻塞syscall不是yield。来源核只允许该owner恢复或更高优先级用户抢占。真正阻塞、yield、RR片尾、降为OTHER、移除来源affinity或退出会结束资格；多个被抢占者可同时保留，按原优先级/FIFO组织。CPU0另有服务资格索引，避免本核高优先级用户计算饿死异核服务；服务完成后重新检查用户资格，不把首次准备完的子任务直接送到U-mode。两种节点均借用既有执行/runnable owner，回收前必须摘尽；不固定MM/files，也不引入worker。

RV整数ABI尺寸探针：实时资格修复使USER_SMP任务由2816增至2896字节，仍由一张4KiB页容纳；每CPU记录由2048增至3664字节，服务索引另为1616字节。每次调度选择O(1)，入/摘维护两CPU及服务索引，现有优先级桶最坏检查仍最多99级，不扫描等待任务。默认及受控SMP构建没有这些新增字段。

服务内wait finish另用真实在途借用及两线程握手核对归还；用户yield与通用内核yield分开，不把等待清理改成延迟的用户轮转。

普通MM的`remote_user`能力与受控并行修改能力独立。ELF、系统后备和压力hook仍可用，VMA修改、实际缺页和文件后端业务仍由CPU0执行；本地驻留复制复用既有operation gate和pin登记，使fork、降权与写pin排空遵守同一协议。撤映射、降权、fork/COW、截断和缓存撤写先更新翻译、固定退休owner，再完成本地及远端确认；新父项/摘空表同步全范围。稀疏撤映射按owned leaf/页表分支遍历，不枚举空虚拟页。最终MM释放要求活动CPU、pin和退休批次归零。

```sh
make kernel-rv-user-smp
make test-user-smp-host
make test-user-smp-riscv INIT_CONFIG=config/init.json
make test-user-smp-runtime-riscv INIT_CONFIG=config/init.json
make test-user-smp-programs-riscv
make test-user-smp-composition-host test-user-smp-composition-riscv
make run-user-smp-riscv
```

2026-10-14的同ELF原生验证使用`tests/smp/linux-user.config`，固定Linux commit不变但单独启用SMP并生成配置哈希/缓存。MTTCG两核及一核退化、512MiB/1GiB完成独立进程无yield计算、pthread/TLS/全部F/D与FCSR、嵌套信号、远端撤权限/文件EOF、pipe取消、非leader exec、组停止/继续、CPU0/CPU1 FIFO服务/yield/阻塞与RR片尾，以及CPU0高优先级用户下的异核服务前进；runner核对完整内容流、真实U-mode hart采样、CPU1不进入通用syscall业务分派、PID1/堆/根盘退出和磁盘检查。host另保护通知消费后的新发布、旧栈owner、晚加入活动MM、COW失败回滚与64GiB稀疏撤映射。静态/动态musl原生公共案例（动态形态另含dlopen TLS）、glibc五形态及1400条原ABI完成SMP Linux同ELF对照，覆盖2/1CPU和双RAM；原static/pthread musl集成consumer在BoarOS同矩阵复核；这部分不称Linux差分。原单CPU策略用例显式固定CPU0，离线affinity位按真实拓扑选取。Ubuntu24.04/QEMU8.2.2本地容器完成原生同矩阵，实际固件/工具身份由runner保存；这不是托管CI运行结果。默认RV/LA core/runtime/platform和原受控SMP组另行回归。

### 用户态原程序与压力组合

`tests/program-inventory/run.py --execution-profile rv-user-smp`仍使用固定229项原程序及原ELF、参数和环境，但Linux参考单独启用SMP。2026-10-14最终内核下，512MiB/1GiB与2/1 CPU四个profile各229项全部通过；本地严格运行使用`--require-pass`，没有reference-not-pass、超时或未到达项。`native-extended.yml`配置相同四格的独立定时job，托管job尚未运行不能由本地结果代替。

`test-user-smp-composition-riscv`把共享匿名/文件映射回收、pipe/futex等待与取消/组退出、SQLite DELETE/WAL及独立重启放进Linux/BoarOS组合。MM ELF有19个有序功能案例，包括匿名和文件共享页的并发首fault，以及普通用户驻留读、冷缓存、用户fault、精确EFAULT前缀、共享位置与seek、close复用和信号、fork排写pin；顺序整页读另检查可选预读及seek取消。Linux和BoarOS功能profile使用同一ELF。BoarOS功能profile启用只读观察钩子但不下达压力/OOM/RFENCE命令，受控profile才运行CPU0真实页账本、目标操作OOM、退休同步暂停和等待登记控制。加压前准备未映射的干净observer文件页，回收统计覆盖账本分配和显式回收，并等待后台轮次发布实际回收数；不要求同步调用抢在后台worker之前回收候选。普通读控制器在真实ecall的复制及迁移点按hart记录字节，先等竞争seek或fork实际park再释放写pin；两hart要求本地复制、CPU0续读和唯一offset借用均发生，一hart仍核对内容与实际pin等待。用户页fault、PID1退出、磁盘状态和SQLite独立重启另有逐项检查。测试控制器、linker wrap及测试宏只存在于私有构建，不进入默认/正式USER_SMP内核。

普通读的聚焦原生验证以512MiB、2/1 hart运行同一19案例ELF的Linux参考，以及BoarOS功能/受控profile。双hart九次受观测操作共复制130字节，其中hart1为84、CPU0为46，确认五次实际任务服务迁移及一次持offset续读；竞争seek必须看到整次读取后的16，fork必须等真实写pin归还后才发布child。无观察钩子的正式USER_SMP内核另运行七项普通读场景及RA2顺序预读/取消，检查内容、自然退出、堆owner和fsck；它验证正式路径功能，不替代受控复制位置与等待交错证据。

同一共享匿名页的双线程首缺页被加入确定性交错：双CPU时两个真实用户任务固定到不同逻辑CPU写同一未驻留页，首个resolver让出CPU0服务资格，直到第二个resolver已提交可写PTE后再继续；单CPU退化时两个任务同绑CPU0。旧路径此时返回`ADDRESS_SPACE`并fatal；固定Linux `mm/memory.c`的`fix_spurious_fault()`（commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`）也将并发页表更新后的fault作为spurious fault处理。现在对`remote_translation` MM在VMA权限仍允许且当前PTE已满足请求时成功返回，不重复发送页级RFENCE：PTE提交者完成其同步，迟到fault返回用户态前的MM激活完成本地刷新；VMA拒绝或PTE权限不足仍走原拒绝/COW流程，普通单核MM原错误检测保持。测试要求两次真实fault、一次stale retry、两次成功及一次页级同步。该行为见[MM硬件缺页解析](kernel-mm.md#硬件用户缺页解析)。

文件fault另覆盖`resumed=1`重试：第一个读fault取得缓存页引用后停在PTE提交前，第二个CPU上的reader映射同一`MAP_SHARED`页并完成同步；第一个继续时看到已有PTE，真实进入一次内部retry，再次解析同一权限。旧实现对该权限已满足的`resumed` PTE仍发第二次远端同步。测试红灯观测`arrivals=2, retries=1, resumed=1, resolved=2, syncs=2`；当前远端MM保留publisher的一次同步，非远端MM的既有resumed刷新不变，绿灯要求`syncs=1`。握手在inode共享读保护下让第二reader继续，持页引用和调用栈覆盖第一次准备；测试同时核对返回数据和自然退出owner。

CPU0最后一条启动记录在`riscv_timer_start()`开启IRQ前完成，避免逐字console输出被已运行用户的TTY数据打断。私有测试在IRQ已开时暂停timer横幅，直到UART实际接纳用户队列数据，旧顺序会破坏横幅完整性；当前顺序不进入该暂停，完整启动记录先于用户案例。runner仍严格拒绝损坏的用户协议行，不从程序退出0推导输出通过。

2026-10-14本地QEMU11.1.2下最终12案例ELF完成60次组合；同一ELF和固件在Ubuntu24.04/QEMU8.2.2容器也完成60次。四个CPU/RAM profile的匿名、文件竞争都要求分别两次fault、一次stale/retry及一次页级同步；受控MM退出owner闭合。QEMU8.2.2最终矩阵的2 CPU配置`remote-retirement=1`、1 CPU配置为0。两套环境分别记录QEMU、kernel compiler、Linux config、firmware、kernel及ELF身份，不能混成一次对照或据此声称性能提升。容器使用独立`--build-root`，防止不同交叉编译器共用对象缓存。

```sh
python3 -B tests/program-inventory/run.py --arch riscv --execution-profile rv-user-smp \
  --cpus 2 --cpus 1 --memory 512M --memory 1G --firmware /absolute/fw_dynamic.bin \
  --reuse-builds --require-pass --output build/user-smp-programs-run
make test-user-smp-composition-host
make test-user-smp-composition-riscv QEMU_RISCV_FIRMWARE=/absolute/fw_dynamic.bin
```

普通用户的驻留复制复用任务已持有的MM/files句柄、实际OFD引用与既有operation gate，不为每次复制建立第二组业务owner。来源退休节点保留原有OFD/ELF引用，放开MM gate并排空登记回调后才执行真实关闭。

服务统计区分CPU1→CPU0交接等待ticks、含阻塞的服务elapsed ticks、按实际执行owner结算的running ns；整数时钟单位与运行耗时不同。观测常驻CPU/任务字段，不分配或增加worker；默认单核配置不含这些字段。该配置不表示ext4、设备、协议、命名空间或系统压力回调已在两核并发执行，LA仍为单核。

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
受控任务只能进入已绑定的MM和获准FD域（匿名pipe及下述预开普通OFD），不得进入一般命名来源、socket/epoll/FIFO、设备、协议或系统压力回调；并行allocator/MM、pipe与受控缓存使用CPU0预留、无压力hook的私有arena。
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

`test-mm-file-smp-riscv` 使用真实私有 tmpfs、控制者OFD和 arena 内的后备预算，在512MiB/1GiB的MTTCG两核与一核退化验证源准备后截断、EOF/重新增长、跨根共享、私有COW、受保护叶、pin迁移及RFENCE后回收。独立RFENCE错误核对来源/登记/数据/表owner保留后fatal，不记自然回收。`test-mm-file-smp-userland-riscv` 让CPU0原静态/pthread程序与CPU1受控任务同时运行；本受控组合把用户固定CPU0，真实用户两核使用USER_SMP；普通OFD接纳另见文件交接章节。接口和关闭协议见[MM](kernel-mm.md#受控-tmpfs-文件映射)。

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
接口、结构成本和竞态背景见[MM](kernel-mm.md#受控-ext4-文件映射)、
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

`USER_SMP`的真实用户`ecall`只为普通磁盘文件的`read/pread/readv`保留本地尝试。当前任务借用自己已持有的MM/files句柄，在原FD raw保护下取得实际OFD pin；关闭和编号复用不改变这次读取的文件身份。每批在同一缓存metadata保护下检查尺寸、代次和全部缓存项，成功取得独立物理页引用后才复制。用户目标页只尝试已有可写PTE，不分配、不处理COW、不发起缺页；共享磁盘映射目标沿CPU0原有路径处理。普通缓存的索引发布和摘除使用同一mutex，CPU0保留原IRQ排他；本地尝试不改LRU或entry借用链。

缓存缺页、用户页需fault/COW或保护正在变更时，先归还整批缓存引用及用户pin，再把原任务的栈和未完成游标迁到CPU0。`read/readv`始终持有同一个OFD offset guard，CPU0只借用该资格，完成后由原调用现场归还；已复制前缀与后续范围之间不允许另一reader或seek插入。`pread`不修改共享位置。迁移入口只特许唯一的指定offset guard，其他锁、用户pin和缓存写借用仍禁止跨交接。最终短fault只推进真实复制前缀；正常缓存边界不产生额外短读。

非末次OFD pin可在本地原子归还。若并发close使它成为最后一个引用，读取者保留这份实际owner并到CPU0完成原后端清理。信号、重启、组停止/退出和返回准备继续使用原服务协议，pending通知使本地尝试转入CPU0。上述普通对象同步仅在`USER_SMP`编译启用；默认单hart和原受控域的接纳条件保持各自规则。

### 受控文件任务

受控FD表可接纳同域、预先打开的tmpfs/ext4普通OFD。CPU0私有校验真实来源后发布稳定资格，控制引用覆盖任务、请求和MM来源；末次后端释放仍由CPU0完成。tmpfs读写及定位操作就地执行；ext4 read/readv/pread在调用hart取得热缓存并复制用户数据，只有miss交CPU0装入；写入和元数据操作仍排入既有cache worker，不改变任务或MM绑定。共享offset由调用者在排队前取得原有锁，worker借用资格并在实际完成后交回，避免队首操作等待本地读者而挡住其miss。fsync/fdatasync/ftruncate及共享错误观察保持相同执行规则；tmpfs截断仍由CPU0控制者完成。路径、statx、socket/epoll、记录锁和sendfile不随FD接纳开放，入口在发布或用户输出前返回STATE。

```sh
make test-file-io-concurrency-host
make test-file-io-smp-riscv QEMU_RISCV_FIRMWARE=build/data-path-baselines/sync-b3/firmware.bin
make test-file-io-smp-userland-riscv INIT_CONFIG=config/init.json \
  QEMU_RISCV_FIRMWARE=build/data-path-baselines/sync-b3/firmware.bin
```

产物默认`build/riscv-file-io-smp-test/`，可用`FILE_IO_SMP_BUILD`隔离另一工具环境。2026-10-13，本地QEMU11.1.2/OpenSBI1.8.1及Ubuntu24.04/QEMU8.2.2/其实际安装固件各通过2/1CPU、512MiB/1GiB的新盘和独立重启8次，以及双RAM的原静态/pthread组合；实际MTTCG线程、退出状态、页/堆/任务栈/等待、私有挂载和两盘fsck由runner核对。本地容器验证不表示远程CI已经运行。

宿主4/16KiB、2/4线程核对共享offset及独立OFD整次追加的内容隔离；强制暂停实际执行者后close/dup复用、取消排队者并停止worker，原请求仍读原OFD，运行者完成前不得join。真实客户机同时验证磁盘映射作为缓冲区的冷缺页、精确短读写内容/哨兵、tmpfs本地I/O和最后控制引用结束后的arena基线。本受控配置的普通用户仍只在CPU0，USER_SMP另行验收。

热读不创建执行请求，用户copy仍在调用hart；冷请求只排入缺失页，不把整个文件操作转交。运行时时间戳由挂载点raw锁保护共享值与dirty/staged链，锁外唤醒和归还节点；读取不为atime单独排队。可选预读的OFD预测用已有短锁，队列由缓存保护；取消只标记，CPU0预读worker在锁外归还节点。

2026-10-16本地QEMU11.1.2分别以RA0、RA2完成受控文件任务的2/1hart、512MiB/1GiB新盘及独立重启。两hart日志要求实际用户copy发生在hart1，热读requests_queued不增加；冷缺页由CPU0加载且不增加整操作执行请求。共享read/readv、短fault与offset、close/reuse、同步和最终owner继续检查。RA2另通过1/4KiB和held DMA检查。该证据来自受控内核文件入口；真实用户`ecall`使用前述驻留尝试与实际任务迁移路径。
