# RV 受控两核调度

默认构建和LA仍为单CPU。`kernel-rv-smp`独立配置最多两个逻辑CPU，CPU0是固件选择的boot hart，
CPU1运行显式创建的受控内核任务，可绑定匿名及私有arena tmpfs/受控ext4文件映射MM，也可绑定同域共享FD表执行匿名pipe I/O。CPU0保留文件构造、读写/截断、MM/fork发布和最后后端释放；控制者保留独立OFD、挂载和缓存owner，命名空间运行期间不修改。用户任务、现有worker、实际磁盘I/O、未开放外部后备和系统压力回收仍只在普通CPU0服务上下文执行。
受控任务已验证 fork 后跨 MM 的共享匿名/真实tmpfs/磁盘缓存页与私有COW；普通用户线程跨核执行及其共享 MM ABI 仍未开放。

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
受控任务仅能进入已绑定的MM与匿名pipe文件表域，不得进入一般文件、socket/epoll/FIFO、设备、协议或系统压力回调；并行allocator/MM、pipe与受控缓存使用CPU0预留、无压力hook的私有arena。
`kernel_thread_create_joinable_mm()`在最终 ONLINE 复查后才转移 caller MM，失败不发布 join 或身份；缓存 root 不取得额外引用。当前任务借用覆盖执行，最后 MM 释放由 CPU0 可信栈的统一 join/reaper 路径完成。活动 MM 借用归零也是 CPU 停止条件。

## 验证入口与证据边界

```sh
make test-smp-host test-mm-pin-host test-mm-concurrency-host
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

`test-mm-file-smp-riscv` 使用真实私有 tmpfs、控制者OFD和 arena 内的后备预算，在512MiB/1GiB的MTTCG两核与一核退化验证源准备后截断、EOF/重新增长、跨根共享、私有COW、受保护叶、pin迁移及RFENCE后回收。独立RFENCE错误核对来源/登记/数据/表owner保留后fatal，不记自然回收。`test-mm-file-smp-userland-riscv` 让CPU0原静态/pthread程序与CPU1受控任务同时运行；普通用户跨核及一般文件/FD业务仍未开放。接口和关闭协议见[MM](kernel-mm.md#受控-tmpfs-文件映射)。

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
默认RA0/WB1保持，SMP+COST继续拒绝；实际QEMU/OpenSBI身份由runner保存。
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

实际登记后的提前wake、fd复用后旧请求、EOF/信号仲裁、blocked/off_cpu迁移、两种任务创建OOM和pipe2输出跨页fault均核对原请求内容及owner。持续4KiB包由CPU0写入、CPU1读入真实受控磁盘映射，冷缺页由CPU0 worker前进；原用户组合要求init实际执行且仍存活期间有包接纳。宿主另使用真实tmpfs映射作为缺页读写缓冲，覆盖4/16KiB几何。普通用户仍在CPU0，未开放socket/epoll/FIFO或文件后端业务跨核。

自然退出同时核对PID1状态、FD/OFD/端点/等待、MM/pin、页/堆/任务栈、私有挂载与根盘设备owner，并运行两盘fsck；不是只检查串口marker。RV产物FD record160字节、pipe504字节、task2784字节，前两者保持256/512字节slab；复用task已有files字段，未新增任务页。扩容锁内复制上限8192字节，pin/close不分配或全表扫描；没有新增数据中转缓冲或worker。OFF工作tick与收尾分别保留，不据此宣称性能提升。
