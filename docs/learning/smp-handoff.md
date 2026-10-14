# 两核启动与执行交接

## SMP 原程序参考环境

原程序driver在执行案例前准备proc/sysfs、tmpfs、mqueue、devpts和loopback。
2026-10-14首次用户SMP清单试跑的两项程序在BoarOS完成，但Linux未启用
`POSIX_MQUEUE`和`UNIX98_PTYS`，准备返回ENODEV，报告为reference-not-pass。
这发生在原程序执行环境，不能归为BoarOS装载或程序错误。
固定`references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`
的`init/Kconfig`与`drivers/tty/Kconfig`定义对应能力。
`tests/smp/linux-user.config`现保留原单核清单所需设施并启用SMP；配置内容进入
Linux缓存键，旧reference及原失败报告不会被覆盖。相同两ELF的新profile复核通过。
host反例约束原程序参考设施不丢失；完整清单结果另记，不由试跑推定229项通过。

## 文件表与端点的身份交接

FD编号只标识当前槽，不能覆盖在途I/O。固定Linux `references/linux/fs/file.c`与`fs/pipe.c`（commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`）分别提供文件取得/替换与管道片段、等待及断端行为依据。BoarOS在FD raw内取得实际OFD引用，锁外复制/睡眠；close摘槽后，旧请求仍持原端点。目录路径也遵循同样交接：OFD pin归还前必须先取得独立path引用。宿主反例让另一关闭消除槽owner，并且root/cwd不额外持被观察目录path，保护相对dirfd和AT_EMPTY_PATH两条路径。

独占record不等于独占其OFD，更不等于独占pipe。普通fork的另一张表可以持同一OFD，重开也可以产生另一个端点或观察者。因此私有启用先核对本表槽数与各OFD引用，再按不同OFD计算端点，最后确认pipe没有域外等待登记/借用；全部通过才sealed。检查使用既有owner，不增加永久反向索引或引用层，最多O(capacity²)，只在未发布构造期执行。后来安装的新pipe同样必须证明未外借，sealed pipe不进入普通表或未同步epoll。

复制mutex保护片段选择到接纳，短metadata锁保护端点与readiness；等待在条件保护内登记，释放二者后park。关闭者在通知完成前仍计入原owner，防止另一末关闭者释放通知目标。16-bit合并位图只改变表示：sendfile来源始终使用新片段，不能并入user-write尾片段。LA真实sendfile组合暴露过遗漏的来源条件，4/16KiB宿主窄反例与双侧同ELF对照保护恢复后的边界。核心返回已接纳前缀和独立断端事实，由有真实用户上下文的调用层发送SIGPIPE，内核任务取得EPIPE/前缀。

当前结果与复建入口见[文件模块](../modules/kernel-files.md)和[受控两核](../modules/riscv-smp.md#共享-fd-表与匿名-pipe)。这些保护不使一般文件后端、socket/epoll业务或普通用户线程自动具备跨核能力。

## CPU 启动与栈交接

HSM start返回成功表示固件接纳请求，不保证目标已经进入内核。固定OpenSBI v1.8.1的
`references/opensbi/lib/sbi/sbi_ipi.c:sbi_ipi_send_many()`只接受当前可中断的目标集合；
`sbi_ecall_hsm.c`与`sbi_hsm.c`分别规定请求入口和状态转换。
基于commit `74434f255873d74e56cc50aa762d1caf24c099f8`，首次真实启动复现：
紧接start发IPI返回`SBI_ERR_INVALID_PARAM`(-3)，目标尚处START_PENDING。
内核先等待CPU1发布本地初始化完成，再发IPI，最后由timer/IPI双握手发布ONLINE。
原失败和修复验证限定于启动时序，没有借此改变IPI失败的fatal契约。

跨核调度需要区分候选、执行owner和旧栈存活。统一锁内的候选摘除与on_cpu获取是一个发布点；
context/FPU尚未保存时，旧任务即使已被wake，也不能重新发布为可执行。
旧任务的on_cpu由新栈完成点释放，然后才发布ready_pending或允许可信栈回收。
全局runnable只持一份成员owner，每CPU资格节点借用这份成员，并不另加引用。
这一结构提供有限CPU资格选择，不意味着共享MM、文件或设备业务数据已有同步。

内核任务没有用户PID，deadline索引不能只用(deadline, tid)作为唯一键。
四个tid=0任务登记相同期限，经AVL旋转后，按原比较器删除可走错分支并触发fatal；
生产wait.c链接的host反例已复现。加入任务存储地址作为最后tie-break后，任意同期限节点仍可精确摘除，
不改变具有唯一TID的用户任务排序；任务owner覆盖节点在树中的寿命。

本地SFENCE只影响本hart。内核栈及空页表的归还顺序是清PTE/摘父项、失效、确认、释放；
固定OpenSBI的`lib/sbi/sbi_ecall_rfence.c`和`sbi_tlb.c`实现同步远端确认。
任务页与槽位不能在确认前复用，失败也不能按普通资源耗尽返回后继续释放。
当前映射软件writer仍只有CPU0，缺表分配可能睡眠，所以候选保持私有并在恢复后重新解析父项。

范围失效与非叶变更不能混用。固定`references/riscv/riscv-privileged-20260120.pdf`
（Privileged Architecture 20260120，SHA-256
`d0f818af6fa519d39e68f822aa795bff9f38032a2f352afdf43e91c0d480e408`）的SFENCE.VMA规则要求：
非叶PTE变更采用`rs1=x0`；只指定叶VA不足以覆盖缓存的非叶翻译。
栈窗口发布新的level-0表或摘空表，因此同步全范围；已有父表内的叶更新可按地址同步。
真实反例检查新父项的RFENCE参数，另由CPU1缓存旧别名，撤销后换物理页，核对新内容；
调用者保持新旧物理owner直到确认，不能通过旧页恰好未被复用掩盖陈旧翻译。

私有资源准备与最终接纳要分别验证状态。一次强制交错让普通CPU0创建者在分配前park，
另一CPU0服务任务确认CPU1无执行/就绪/已发布owner并完成HSM停止，再恢复创建者。
旧路径沿用准备前的ONLINE快照，最外层raw释放后向STOPPED hart发IPI，OpenSBI返回-3。
最终rank40接纳点重查CPU资格后，失败锁外归还私有栈、表及metadata，join保持未接纳。
这不需要把私有构造加入已发布任务计数，也不需要新增worker层。

归还最后存储借用不等于当前上下文有资格收集。受控任务可以在CPU0归还借用，
但业务资源清理仍由普通CPU0服务执行；旧路径仅判断CPU编号，会撞上collector的受控上下文断言。
现在CPU1与受控CPU0都保留真实退休队列owner，普通CPU0才内联收集。
反例在put后的IRQ关闭区检查owner，再核对join后的基线，避免把后续合法idle收集当作提前释放。

这些反例、双向迁移和组合程序均有真实MTTCG双RAM结果，支持受控两核调度核心的正确性闭环；
原调度证据不覆盖共享用户MM、设备业务并发或LA多核。受控私有匿名MM的新证据及边界见[内存许可与失效完成](memory-management.md#私有匿名并发的许可与失效完成)。

生产接口和当前验证边界见[RV两核模块](../modules/riscv-smp.md)。

## 描述符编号与请求身份

FD 槽只有在查找和取得 OFD 引用由同一把锁保护时，才能把编号转换为请求 owner。
close 只摘下当前槽 owner，正在执行的读写继续使用自己的 OFD；不能再按 fd 数字重新查找或取消。
成对 fd 的用户输出属于发布前准备：编号先预留，复制成功才安装两个 OFD，失败只取消这次预留。
这样避免复制期间另一个执行者关闭、复用编号后，失败清理误关新对象。
扩容把 allocation/zero 放在锁外，重查后发布；旧数组不被裸槽指针借用，因而解锁即可归还。
固定依据是 `references/linux/fs/file.c`，Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`；
其摘槽与锁外关闭关系用于核对语义，BoarOS 的短锁和预留表示由本模块自己实现。

管道关闭的端点人数和存储owner有不同的结束点：先更新readiness并通知，通知仍使用管道，所以关闭者的已有owner保留到通知返回，再归还存储引用。
若先减至零、解锁后通知，另一个关闭者可能在回调前释放对象。
复制资格与短metadata保护也分开；poll回调只读metadata，不等待复制mutex，用户缺页不会占用raw锁。
等待token在metadata保护内登记，保护释放后才park，提前通知由已有单次仲裁接纳。
bootstrap阶段调度器尚未启动时，合法非阻塞管道没有可park任务；NOT_INITIALIZED通知与真正的运行期错误分别处理，最终queue销毁仍核对注册/借用归零。

普通文件的CPU0交接必须先固定OFD，不能把fd数字排队后再查。对象锁也由实际执行者取得；提交者持offset或写门等待worker，会与worker取得同一资格形成等待环。
已有cache worker可借用可恢复调用现场的参数完成整次操作，不需要复制用户数据到另一个请求缓冲或增加线程。普通CPU0 worker不绑定用户MM，冷缺页须使用已启用并发、持gate的owned PTE路径；旧“目标satp必须当前”检查不能直接用于这类物理页访问。
这个例外只用于受控fault，用户/受控任务及普通MM仍验证活动地址空间。来源、VMA代次、EOF和权限的睡眠后重查及RFENCE退休契约不因此减少。
OFD同步错误观察使用rank10 offset保护后，所有并发msync都必须先固定来源并结束MM gate25。仅给磁盘来源释放gate会让tmpfs在25→10处违反锁序；真实tmpfs共享映射反例定位并保护了此边界。
固定依据为本地`references/linux/fs/file.c`、`fs/read_write.c`及`mm/filemap.c`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`；分别核对共享位置、接纳字节和同OFD错误观察。执行适配保持单次操作语义，内部队列与owner由BoarOS维护。

RV PLIC的dispatching由CPU0派发器独占。原查询直接返回全局值，会把CPU0正在处理设备IRQ误报为CPU1的本地IRQ，后者即使没有锁、pin或抢占禁用，也被请求入口拒绝。真实两核原静态程序组合捕获了CPU1读到active并返回STATE的反例；查询现在先核对调用核，CPU1不读取该CPU0独占标记。这个修复只对应当前设备IRQ归CPU0的平台边界，不声明PLIC已支持多核设备派发。

## 实际调用任务与用户执行资格

普通用户的CPU0交接迁移调用者本身，不能让worker替代它处理信号、身份、restart、exec或线程退出。trap frame与C调用现场都留在该任务栈；旧CPU完成保存并结束执行owner后，CPU0继续同一调用。临时内核资格与请求affinity分开，才能让CPU1-only用户线程在CPU0调用syscall，同时仍向用户报告CPU1。

纯timer/IPI不是进入业务层的理由。远端只读取原子待服务位和调度元数据；完整信号/组状态仍由CPU0读取。先消费通知再处理，恢复过程不能清掉之后到达的新通知。host握手在消费后再次发布，验证恢复用户资格仍保留新位；实际信号、取消和非leader exec则由同ELF原生对照验证。

普通MM远端执行也不等于并行修改。短保护只协调活动根与退休，既有CPU0缺页/来源策略继续复用。普通COW原先只做本地确认；新增反例要求旧父PTE页引用在远端确认时仍在。新页访问失败必须完整回滚而不是fatal；新增非叶页表还必须全范围失效，不能仅同步目标叶。64GiB稀疏撤映射只遍历实际叶及分支，避免把能力扩展变成空页线性扫描。

固定本地依据为`references/linux/kernel/sched/core.c`、`arch/riscv/kernel/entry.S`、`mm/memory.c`和`mm/filemap.c`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`；用户可观察affinity、trap保存及COW/来源边界按这些路径核对，BoarOS的CPU0交接由自己的执行owner协议实现。SMP reference采用独立配置与身份，不沿用单CPU Linux结果。

实时用户资格不能由临时服务迁移隐式让出。固定Linux `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `kernel/sched/rt.c` 在 `yield_task_rt()` 显式移到同级队尾，非阻塞syscall不做这种移交。同一个原生ELF把父子固定CPU1/FIFO40：Linux在父yield前不运行子，旧CPU0交接实现则提前运行子。修复在实际任务的来源CPU保存实时资格，并在CPU0独立索引内核服务；阻塞/片尾等真实资格结束点释放。只优先服务还不够：同级子任务完成首次内核准备后，恢复用户态必须重新服从来源资格，CPU0也适用。原生测试同时保护非阻塞、yield、阻塞、RR以及服务前进性；宿主模型另检查嵌套优先级保留及节点归还。

普通用户远端执行使失败回滚也成为硬件并发边界。旧Sv39缺页先发布L1、后分配L0，第二次OOM会清父项并直接释放L1；另一hart已在途的页表访问不能由“最终映射失败”取消。现在整条缺失路径私有准备完才发布父项，失败仅释放私有候选；已发布非叶仍遵守全范围失效后退休。宿主用实际MM/Sv39与第二级OOM握手，让另一CPU在准备期间观察根，保护这一owner边界。组停止也必须在最终状态提交锁段核对实际执行owner，不能沿用解锁前的READY判断；真实claim握手反例和用户SIGSTOP/SIGCONT保护停止通知不会在已消费组信号后丢失。

受控kernel FPU探针不能留下没有任务权威图像的Clean状态。`riscv_fpu_state_restore()`以Clean结束，生产恢复使用任务自有图像并处于切换保护；探针却恢复局部expected后才单独标Dirty，间隙timer可以运行同核伙伴。强制tick在该处复现了另一任务的完整寄存器图案；探针现以IRQ保护包住恢复与Dirty发布，`test-smp-fpu-setup-riscv`封存独立核并保护8个实际owner，所有32寄存器/FCSR比较继续执行。用户态FP指令的硬件Dirty更新与本测试手工发布不同。最初guard启动仅有`preempt-FD`阶段、没有实际寄存器值，不能仅凭后续通过唯一归因；其原核与失败现场保留，新增反例明确证明并修复这一初始化缺陷。

FIFO的非阻塞窗口须先区分真实睡眠。动态musl首次fork返回/时钟库代码在BoarOS冷ELF缺页路径会等待CPU0 I/O；服务跟踪确认cause=instruction-page-fault并提交真实阻塞，释放实时资格合法。原生策略案例先预热fork返回、时钟与syscall代码，再校验无主动yield的getpid窗口；真正waitpid阻塞另测，不把cold fault解释成非阻塞调度错误。预热后的同ELF在旧核仍能检出FIFO资格缺陷，动态Musl及其原TLS DSO以独立同ELF profile对照，原static/pthread consumer的BoarOS集成不冒充Linux差分。

## 用户态原程序与生命周期组合（2026-10-14）

用户SMP原程序在独立Linux SMP profile下重跑原229项，四个profile（512MiB/1GiB × 2/1 CPU）各229项全部通过。固定输入是Linux `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`，配置文件`tests/smp/linux-user.config` SHA-256 `dac55880b20f0091d1cfd4d86ee619456a1cdcc7177e8611030f107bf36c9516`；本地冻结USER_SMP内核SHA-256 `9fe57b5d6f806f944b455b16abc9b139643ce2539ff0e1dba29bb9a8315fde04`。四个profile逐项报告都标为complete；严格入口为：

```sh
python3 -B tests/program-inventory/run.py --arch riscv --execution-profile rv-user-smp \
  --cpus 2 --cpus 1 --memory 512M --memory 1G \
  --firmware build/data-path-baselines/sync-b3/firmware.bin \
  --reuse-builds --require-pass --output build/user-smp-programs-run
```

同一USER_SMP核的静态与动态musl原生用例各在Linux/BoarOS、1/2 CPU、512MiB/1GiB通过；1400条ABI manifest、原static/pthread/TLS consumer和glibc 2.44五种形态在同一矩阵通过。默认单核program inventory先前的RV 512MiB `du`/动态`/proc`参考失败仍留在其原记录中；独立Linux SMP配置的229 pass不回写为单核参考结果。

有限组合用同一静态ELF（SHA-256 `260aac9ad6941ac3b2be05e07b379c326e49491b51eaff4bdc2db885b0cb1232`）完成60次本地QEMU11.1.2启动，四个CPU/RAM profile各15次：共享MM/文件映射及回收、匿名/文件首缺页竞争、1/8/32 pipe/futex等待与退出、SQLite DELETE/WAL及各自独立重启。Linux和未施加压力的BoarOS运行共同功能；另20次BoarOS受控构建持有真实分配页账本并检查退出归还。四次MM压力运行均记录页数`held==released`、一次真实页缓存回收、一次目标OOM、一次退休同步暂停和123个PREPARED等待登记，最终ledger/control均为0。远端目标计数核对为：双CPU两配置`remote-retirement=1`，单CPU两配置为0；不能把单CPU本地SFENCE暂停称为RFENCE。60项均退出0、无超时；独立e2fsck与SQLite读回通过。pressure/OOM/退休暂停只用于BoarOS内核test build，Linux只承担同ELF可观察行为对照。

首缺页组合捕获了两个用户SMP MM边界。匿名情形下，两个线程对未驻留的`MAP_SHARED|MAP_ANONYMOUS`页同时写，测试等到第二个resolver已提交可写PTE后再恢复第一个硬件fault；旧路径看到权限满足却返回`ADDRESS_SPACE`并触发S-mode fatal。文件情形下，第一个`MAP_SHARED`文件读fault取得页引用后在inode读保护内yield，第二个reader提交同一PTE后，第一个的页后检查设`retry=1`，再次进入resolver时`resumed=1`。旧实现对这次present-PTE又发RFENCE。把测试候选暂时恢复旧条件，红灯为`arrivals=2, retries=1, resumed=1, resolved=2, syncs=2`；修复后只保留发布者的页同步，重试者重新进入用户态时的MM激活负责本地刷新。非远端MM的原`resumed`本地刷新保留，VMA/PTE权限不足仍走原拒绝/COW路径。固定Linux `references/linux/mm/memory.c`的`fix_spurious_fault()`用于核对并发语义；最终2/1 CPU和512MiB/1GiB四配置都分别验证匿名及文件竞争`syncs=1`，全60次应用组合通过。两个内核resolver仍按CPU0服务协议串行，不能据此声称页表由两个CPU并行修改。

原冻结核`4025198ea029fd90341579958b674c1d11789ec958b8bbc8047e27512dc9020a`的四格矩阵完成914项pass、2项protocol-error。失败是512MiB/1CPU的`libc.dynamic.printf_fmt_g_round`和1GiB/2CPU的`libc.static.string_memset`；两例程序wait状态均为0且根退出完成，但`riscv_timer_start()`开启IRQ后才打印的timer横幅插进了`SUITE ENV`行。原输出保留失败身份，不能因退出0补记通过。强制交错在横幅写出前缀后让UART实际接纳用户输出，复现`timer frequency=COMPOSITION CASE…`，内容及owner仍完整但启动记录校验失败。当前先完成横幅再开启timer IRQ，同一反例通过；parser没有放宽损坏记录的规则。

同一12案例ELF及60次组合在Ubuntu24.04容器、QEMU8.2.2最终同步修正和启动修正下重新全部通过，文件`resumed`重试及实际远端目标计数均与1/2 CPU profile相符。该环境与本地QEMU分列：完整工具容器镜像`sha256:e1c808712f62cb15e4e203c8facf9a8c1767ed601b0cd2349f4a778c562d89d0`；QEMU二进制SHA-256 `49b77f94862a9ae0e6e31d031a31cce9a668d647b5e827d706bf1e9951e96710`，报告版本`8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18)`；kernel compiler为`riscv64-unknown-elf-gcc 13.2.0-11ubuntu1+12`，SHA-256 `8c7a8599f3e56e6abe23d4d54025f77ad03efd4113394eac16f1d6060b626974`；测试firmware仍为QEMU参考OpenSBI字节，SHA-256 `894e2aef99590fc07ec6c60ab00282b8bc5d5d5bb2a1d0c6ada0c52df24274c0`。容器profile Linux config hash为`925564bf8d751433ea3d7c18e848bb99a8588470f1af0d150b53c48d409eb428`，由不同host compiler产生的Linux产物身份单独记录，不能混进本地11.1.2同一次报告。只含交叉工具链的基底镜像`sha256:a5527d29952a18e962815ab3eea8a46448bbaee8d2a83da20d150c384f33a698`缺少`flex`，该次准备失败未启动QEMU；完整镜像的flex/bison/bc身份与封存Linux cache匹配。报告与运行镜像核对后按既有清理规则删除，保留可复用构建、容器和reference缓存；这些不是托管CI结果。

使用已保留的完整工具镜像，可在仓库根目录复建同一容器组合：

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:$PWD" -w "$PWD" \
  sha256:e1c808712f62cb15e4e203c8facf9a8c1767ed601b0cd2349f4a778c562d89d0 \
  python3 -B tests/smp/composition.py --qemu /usr/bin/qemu-system-riscv64 \
  --firmware build/data-path-baselines/sync-b3/firmware.bin \
  --build-root build/riscv-user-smp-composition/ubuntu2404 \
  --output build/user-smp-composition-container-run
```
