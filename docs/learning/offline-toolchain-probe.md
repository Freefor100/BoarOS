# 客体内编译：从小探针到原版工程

宿主交叉编译生成RV64 ELF，只证明宿主工具链和客体执行路径。客体内编译需要把
原生RV64编译器、汇编器、链接器、libc开发文件及依赖安装到同一离线磁盘，实际执行
构建规则，再运行新产物。目标文件存在、版本字符串正确或包装器退出0，都不足以
证明这条链路成立。

固定输入由references/sources.tsv和准备脚本校验。当前为Alpine v3.22的
GCC14.2.0-r6、binutils2.44-r3、musl开发环境和GNU make4.4.1-r3；make许可
GPL-3.0-or-later，Lua5.4.3许可MIT。工具和Lua保持原包/发布内容。
固定Linux依据位于references/linux（v7.2）；精确输入身份归机器清单，人类记录
保留版本、机制、结果与重建入口。

## 小探针怎样发现通用缺口

2026-09-27先建立没有编译器的诊断镜像，Linux和BoarOS均在预处理阶段无法exec，
后续四阶段跳过。这是有效的失败分类，不能称为离线编译成功。

安装原GCC后，cc1以O_NOCTTY打开源文件，原内核的flag校验返回EINVAL；接纳该
通用flag后，又在GCC对链接产物执行fchmodat时遇到ENOSYS，最终exec返回EACCES。
修复使用lwext4的活inode句柄与事务，保护unlink后的fd、ctime及错误owner。
最终预处理、编译、汇编、链接、运行五阶段在两侧均退出0，产物逐字节一致。
2026-09-28的VFS/proc整合后复验保持一致。

默认的小型探针仍由`make test-offline-c-riscv`运行；无工具诊断入口为
`make test-offline-c-baseline-riscv`，两者判定边界不同。新项目选择不会改变默认行为。

## 镜像与参考环境也必须有效

固定Linux的PID1退出以panic停止客体，并没有正常卸载ext4。fsync后的目录项可能
仍位于journal中，直接debugfs查询会误报缺失。执行器先重放日志，再检查一致性；
只有明确属于异常退出的orphan清理才允许一次限定修复，其他fsck修复均失败。
依据为references/linux/fs/ext4/orphan.c。BoarOS则在可信启动栈回收任务和根挂载。

[CI run36](https://github.com/Freefor100/BoarOS/actions/runs/36513240987)中SQLite
通过而离线GCC在根盘第一请求超时。该现场没有请求类型，不能归因于ext4或SQLite。
原复制会把稀疏镜像扩成大量零写；执行器改为保留空洞并在启动前同步副本，镜像
内容和内核一秒超时保持不变。

[CI run37](https://github.com/Freefor100/BoarOS/actions/runs/36515194047)中的新失败
是固定Linux在旧QEMU上SIGILL。相同版本复现和反汇编定位到用户浮点指令；旧DTB
只提供riscv,isa，精简Linux配置未启用ISA回退，未开启用户浮点状态。
启用固定Linux的CONFIG_RISCV_ISA_FALLBACK后，本地QEMU8.2.2与11.1.1均通过。
依据为references/linux/arch/riscv/kernel/cpufeature.c和Kconfig。此结论是本地复现，
不替代新提交在远端CI上的结果。

## 管道与默认FIFO jobserver（2026-10-03）

匿名pipe原先固定返回0600，fchmod返回ENOTSUP。mode、创建时间、ctime和稳定身份
现归共享pipe；两端、dup/fork/proc重开一致，改权保留FIFO类型和原访问方向。
匿名pipe读写不更新文件时间。坏stat指针使用raw syscall，避免把libc转换时的用户
fault误当作内核errno。

命名FIFO的身份、权限与时间归真实ext4/tmpfs inode，传输与等待归pipe。
节点只保存弱关联，每个打开中和已打开的OFD另持节点与pipe。最后owner先摘关联、
丢弃缓冲，再关闭VFS引用，避免循环引用和重复端点归还。硬链接与改名沿用实例，
unlink后已打开实例仍有效，同名重建则隔离为新inode。

固定Linux的fs/pipe.c以到达计数保护“对端出现后立即关闭”。只重查当前人数会漏掉
会合并再次睡眠。BoarOS采用到达代次；初始非阻塞只读尚未见过写者时抑制HUP，
随后写者离开才报告HUP。信号重启、取消和fd满均不残留资格。对象与64KiB连续缓冲
分配失败后可重试，最终归还owner。只读挂载允许FIFO传输，无法取得挂载写资格时
跳过时间更新；节点可以同步持久化，传输内容永不恢复。

同ELF先证伪旧mkfifo行为，再验证两种文件系统、打开会合、poll/select/epoll、时间、
信号、只读挂载和重启。重建`make test-fifo-riscv`；OOM入口为`make test-files-riscv`。
语义依据为references/linux/fs/{pipe,open,inode}.c。

## 原版Lua工程的完整流程（2026-10-03）

使用34个C源文件与原Makefile，在客体运行`make -j1 linux`和`make -j2 linux`。
两侧完成干净构建、无变化重建、仅lapi.c改变的增量、clean后重建、明确语法错误的
非零退出及恢复。对象与lua/luac/liblua.a的内容保持一致；无变化重建不重新编译，
增量只更新必要对象、归档和链接目标。新Lua实际执行文件/模块和受控子进程，luac
完成字节码往返，静态库嵌入程序与C动态模块运行。同步后重启仍可使用这些产物。

默认make jobserver确实使用命名FIFO。递归负载的共享计数证明峰值两份资格，六个
任务全部完成且令牌归还；中断后子进程被回收，GMfifo节点被删除。正式性能输入
不使用这套资格诊断包装器。

参考流程曾因镜像缺/bin/echo失败：make可直接exec简单recipe，因此shell中能执行
某工具并不代表PATH里有对应程序。补齐工具链接后Linux的窄探针和完整流程成立。
错误恢复驱动还使用过未实现的路径truncate系统调用；现用已有ftruncate恢复输入。
这未修改Lua或make，路径truncate仍是能力缺口，不被这次工程通过掩盖。

重建`make prepare-offline-c-toolchain test-offline-project-riscv`。现有小C探针默认
不变；项目执行器记录每条命令的argv、cwd、真实wait status、耗时和产物身份。
运行目录与机器记录在忽略的build，Git保存本记录与可重建代码。

收口审查补到了信号与会合的交错：信号已将打开者唤醒，但它恢复前对端已出现又关闭。
固定Linux仍认领成功会合；旧代码错误返回EINTR，SA_RESTART还可能错过对端再等待。
以不同实时优先级控制单hart交错，先证伪再修复，读写两方向和两类信号均通过。
窄探针使用raw sched_setscheduler，因为固定musl的POSIX同名封装返回ENOSYS，不能
把该libc行为误报为系统调用缺失。

执行器的输入身份也改为在启动前固定：驱动取实际fixture中的init，Linux与BoarOS
均使用独立kernel快照；源commit、源树与未提交patch分开记录。运行结束不再重新
散列可被重建替换的输入路径。确定性测试在模拟启动中替换这些文件，保护这个契约。
