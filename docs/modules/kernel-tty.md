# TTY、串口与作业控制

`fs/tty.c` 是软件行规程，`kernel/sched/tty.c` 连接线程组、稳定 PID 身份、信号与
proc 快照；RV/LA 共用硬件入口见 [UART transport](riscv-uart-tty.md)。生产启动在标准 OFD
建立之前创建、配置并发布根 TTY，默认 canonical、echo、ISIG、115200 8N1、CREAD，
winsize 初始为零。固定设备类别由 `st_rdev` 选择；open 另借用调用期间已 pin 的 VFS file/path，
动态 devpts 实例按节点绑定识别，不能仅靠复用的设备号选择。ttyS0 为 4:64、console 为 5:1、当前
控制终端为 5:0。console 不自动取得控制终端；没有 ctty 的 `/dev/tty` 返回 ENXIO。
未建立 transport 的独立内核 fixture 仍使用原裸 UART console，不能当作 TTY 验收。

## 对象与 owner

根启动拥有 core；每个 OFD 从自己的 heap 创建一个实例并持有 core 引用，dup/fork
共享该 OFD。每个有 ctty 的线程组另持一个 core 引用，线程成员借用 leader 的关联。
TTY 的 SID/前台 PGID 是持引用的 `kernel_pid`，身份成员链只借用 task，不构成引用环。
fork 继承 ctty，线程 clone 共享，setsid 清除本线程组关联，普通 exec 保留；非 leader
exec 转移 leader 的既有 ctty 引用。PID 号码退出后可由真实 SID/PGID owner 继续保留。

会话 leader 可在非 O_NOCTTY 的可读 ttyS0 open 自动取得 ctty，或经 TIOCSCTTY
取得；当前不可变 root 用户模型对应 CAP_SYS_ADMIN 的 steal 路线。TIOCNOTTY 对
leader 清除整个会话关联并向前台 PGID 发 HUP/CONT，对普通线程组只撤销自身关联。
leader 最后成员退出的 serial hangup 向 session leader 发 HUP/CONT，并向前台组发 HUP；
既有实例的 generation 随 hangup 失效，后来的新 session 不会复活旧 OFD。普通 close
不触发会话 hangup，最后一次真实 OFD close 的 HUPCL 由 transport 操作硬件 modem。
proc stat 的 tty_nr/tpgid 来自同一受保护的实际关联快照。
TIOCSPGRP 查找数字身份：身份不存在返回 ESRCH；存在但属于其他 session 或没有活成员
返回 EPERM。按固定 Linux 在无 PGID 成员时检查 PID 成员的 session，成功才更新前台引用。

完整 readv/writev 与可阻塞 ioctl 的调用拥有 OFD pin 和已导入的 heap iov。进入 hook
时从调用者 owner 指针移交，正常返回恢复供原调用者收尾；强制终止则消费这些 owner，
先释放请求的 read/mode 状态和 vectors/OFD pin，再清理 files/MM。请求仍借用退出 task
的栈，因此 reaper 在归还栈页之前执行 abort。OFD 后端真实 close I/O 失败仍交给对应
files cleanup owner，TTY 不引入成功存根或重复释放。

## 行规程与 I/O

输入有独立 4096 字节环；canonical 保留最多 4095 个普通字节加 delimiter，超长普通
输入丢弃而控制字符继续处理，raw 容量为 4095。支持 CR/NL 映射、strip、break/parity
处理与 PARMRK、IXON/IXANY/IXOFF、canonical erase/kill/word erase/literal next/reprint、
IUTF8 erase、EOF/EOL/EOL2、ISIG 与 NOFLSH。禁用 cc 不作为特殊字符；IMAXBEL 与
固定 Linux 一样不产生额外机制。错误 marker 按整个展开记录保留，不发布截断 FF/00。
流控与信号检查在 CR/NL 映射之前，canonical 编辑在映射之后。

readv 的初次资格、64 字节 scratch、read serialization、mode guard、绝对 deadline 与
continuation 属于一个请求。短 publication 区把输入复制到独立 scratch 并同时消费，
随后在 publication 区外 usercopy；fault 返回实际交付前缀或 EFAULT，已消费的 staging
尾部不回滚。只按 continuation 许可继续，不为每个 iov/64B 块重新发起新 read。
canonical 读在 delimiter 处停止，EOF 不交付；容量恰好结束或 fault 后的零长度 cookie
清理按固定 Linux 跳过紧邻 EOF。VMIN 超过 64 时初次 staging 仍可在填满 64 后返回；
这不是把 minimum 偷改为整次用户请求长度。raw 的四种 VMIN/VTIME、O_NONBLOCK、
信号前缀与 restart 分别处理，spurious wake 不刷新期限。

Flush/改 mode 与 read continuation 的 mode guard 串行，等输入时释放 guard，使配置
与 RX worker 可推进。ISIG 的 flush 先发组信号；串口 worker 可等逻辑 guard 排空，非阻塞接收则登记延后 flush，
在 guard 归还后完成，保留后续尚未处理的输入；
等待不持 inode/namespace/device 锁，后续输入不会被先前延后的 flush 误删。前台控制
信号针对整个 PGID；背景读发 TTIN，TOSTOP 写和修改型 ioctl 发 TTOU，blocked/ignored
及 orphan 条件按固定 Linux 返回/放行。partial 结果不留下 syscall restart 标记。

用户 writev 先完整复制最多 2 KiB 的请求暂存块，复制失败不发布该块；已经发布的
完整块构成短写前缀。暂存归请求 owner，取消与 OOM 归还；整次写资格保护并发写入，
不把 2 KiB 缓冲放入 8 KiB 内核栈。用户 TX 与 echo 各有独立 4KiB 容量，合并顺序队列维持已接受输出的先后。echo 以完整
字符/编辑操作提交，空间不足丢弃该完整操作，RX 不等待 echo 空间；software xchar 优先
且不受 stopped output 阻挡。OPOST 支持 ONLCR/OCRNL/ONOCR/ONLRET 与 TAB3，并维护
列位置。transport 只写实际硬件 credit，不串口 busy-wait。shutdown 拒绝新 input/open，
唤醒旧实例并打开停止流控，仍服务已经接受且未 flush 的输出；root 在真实 drain/join
以及所有 OFD/ctty owner 释放后才能 destroy。

TCGETS/TCSETS/TCSETSW/TCSETSF 使用 Linux RV64 **36 字节** kernel termios，不能复制
libc 的更大布局。硬件普通配置由 transport configure 校验并回写生效值；unsupported
软件 bits 清除，TCGETS2/TCSETS2/TCSETSW2/TCSETSF2 使用独立的 44 字节布局，旧 TCGETS 系列仍只复制
36 字节。PTY 保存独立输入/输出速度；UART 按单时钟归一，详见[串口传输](riscv-uart-tty.md)。
其他 ldisc 与完整 modem ioctl 未实现，返回明确错误。真实应用收口状态见[开发路线](../goals.md)。
支持 winsize、TCXONC、TCFLSH、FIONREAD、TIOCOUTQ、TIOCGETD(N_TTY)、前台 PGID/SID
与 ctty 操作。TCSBRK 的非零参数真实 drain；零参数没有 break 生成 owner，返回 ENOTSUP。
IN/OUT/HUP 使用统一事件队列；canonical IN 需要完整 line/EOF，raw poll 按其 minimum/time
规则，hungup 实例返回实际 HUP 与 EOF/EIO。

## 固定依据与验证

固定源码：`references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`，
`drivers/tty/n_tty.c`（canonical/64B cookie、VMIN/VTIME、软件 flags）、
`drivers/tty/tty_io.c`（iterate_tty_read/consume-before-copy/故障 cookie 清理）、
`drivers/tty/tty_jobctrl.c`（ctty、SIGTTIN/TTOU、HUP/CONT）和
`include/uapi/asm-generic/termbits.h`（36B ABI/flags）。

```sh
make test-tty-host
make all
build/riscv/musl-root/bin/musl-gcc -fno-link-libatomic -static -pthread -O2 \
  -Wall -Wextra -Werror tests/tty/probe.c -o build/tty/probe-rv
python3 -B tests/tty/riscv.py --only both --kernel kernel-rv --probe build/tty/probe-rv
```

宿主测试执行真实 core，覆盖软件 flags、限长/原子 echo、deadline/continuation、fault
前缀、可失败分配和放弃阻塞调用栈后的 owner 清理；依赖 transport/scheduler 模型不代表
UART/CSR 或真实用户信号已经验收。相同 probe ELF 在固定 Linux 与 BoarOS 中通过 raw
host PTY 与独立 serial/monitor-off 通道运行，按完整 TTY_REQUEST/FIONREAD/任务 state
handshake 注入；原始字节不做文本归一化。原 BusyBox ash、stty 与完整 job-control 验收
由同一独立 launcher/runner 完成，结果收口在开发路线与 learning，不由宿主模型代替。

## 配对 transport 接缝

TTY core 仍由 transport 的 base owner 管理；外部 OFD/ctty 引用通过可选通知保活
配对，base 引用不反向保活配对。打开上下文只借用已 pin 的路径，实例不借用临时
用户指针。分配完成后在发布区重新核对锁定和对端关闭，防止检查到安装之间的状态变化。

`kernel_tty_receive_nonblocking` 返回已经处理的前缀。raw 容量不足保留后缀；
canonical 超长行仍按行规程处理。普通 RX 不等待 read continuation 的 mode guard，
信号立即交付而必要的 flush 延后。read/poll 可通过 transport 做有界推进，随后按真实
数据、peer 状态和 packet 控制状态判断；无进展必须等待事件。

PTY 两端拥有各自行规程状态；master 初始 raw，slave 初始 canonical/echo/ISIG。
按 Linux 的 mode ioctl 规则，master 上 TCGETS/TCSETS 系列查询和设置 slave termios；
这不改变 master 自身的 raw 输入处理。
设备身份从 core 读取，ctty/proc 不再假定串口设备号。winsize 在配对内同步，尺寸改变
才发送 SIGWINCH；master 的 TIOCSCTTY 和前台组/SID 查询使用 slave 的控制身份。
普通字符节点只有设备号而没有 devpts 绑定时不能打开 slave，返回 EIO；O_PATH 仍可持有节点身份。

TIOCPKT 默认关闭，控制状态优先于数据；正常非 canonical 数据前缀为零，
单字节读取只交付前缀。STOP/START、DOSTOP/NOSTOP 保留最新互斥状态，flush 位
可合并，POLLPRI 只表示实际待读控制状态。EXTPROC 保留必要的输入、EOF 和
termios 通知契约。固定依据另见 `references/linux` v7.2 的
`drivers/tty/{pty,n_tty,tty_ioctl,tty_jobctrl}.c`。
重复停止/恢复不产生新控制包；VSTART、IXANY、信号和关闭 IXON 经统一状态转换，
不能越过 TCOOFF 的停止资格。

LA入口与同ELF两种RAM验收、原script真实重启和UART回收见[共用UART](riscv-uart-tty.md#la-平台与对照验收)。LA/RV均采用asm-generic36/44字节内核termios/termios2；字符设备号和PTY所有权共用。
