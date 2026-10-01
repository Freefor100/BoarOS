# 内核日志与 klogctl

`kernel/log.c`拥有16KiB文本环、512项固定记录索引和1KiB拼接区，从早期启动记录真实消息。
序号、首存活记录、消费游标/记录内偏移及清空边界均为内核标量。短记录较多时索引上限
也可触发整条覆盖；SIZE_BUFFER返回文本环容量，不承诺任何长度的消息都能保留同样条数。
记录最长1KiB，长行分段；默认内核消息为级别6并保存`<6>`前缀。追加不分配、不睡眠，
关中断区只发布有界记录和唤醒队列。UART打印在发布区之外，用户console写绕过日志，
因此读取dmesg不会把同一批消息重新写回环。过滤只控制内核串口输出，不删除记录。

RV64 syscall116接入`kernel/syscall/log.c`。0/1沿Linux保持打开/关闭控制语义；2消费式
阻塞读，3读取清空边界之后能够完整放入缓冲的最新记录，4读取并推进清空边界，5清空，
6/7关闭/恢复console级别，8设置1–8级别，9查询未消费字节，10查询容量。
消费游标和清空边界独立；覆盖使消费游标向首存活记录前进。消费读取用rank1 mutex
串行游标，睡眠前释放mutex并在IRQ保护下挂入发布队列，信号经统一restart协议处理。
全部读取捕获固定结束序号，期间新增消息不无限延长操作；并发覆盖以最新存活序号继续。
用户复制在IRQ保护之外，使用本次调用拥有的1KiB堆scratch；故障不留下堆或锁owner。
消费读在复制前推进本段游标，已完成记录后的fault返回此前字节；全部读的fault返回EFAULT，
READ_CLEAR的失败边界不会把尚未处理的新记录清掉。NULL/负长度为EINVAL，长度零和fault
顺序按固定Linux核对。

权限策略集中在log层：未具备特权只允许READ_ALL/SIZE_BUFFER。当前任务身份仍是不可变root，
syscall传入该真实模型的特权值；这没有新增凭据体系或多用户隔离。控制级别默认7、最小1，
CONSOLE_OFF保存旧级别，CONSOLE_ON恢复，LEVEL隐式重启控制台过滤。

依据：`references/linux@f4cdf7ca9a1fdcca413157df19753f388a5a224e`，
`kernel/printk/printk.c::do_syslog/syslog_print/syslog_print_all/check_syslog_permissions`，
`include/uapi/asm-generic/unistd.h`。验证：`make test-log-host`、`make test-environment-riscv`、
`make test-diff-abi-riscv`；后两者同时覆盖RTC。原BusyBox的dmesg、-r/-c/-n由真实程序清单验收。
