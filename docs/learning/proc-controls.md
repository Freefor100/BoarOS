# proc 控制文件不使用普通 proc 快照协议

固定依据为 `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `fs/proc/proc_sysctl.c:proc_sys_call_handler`、`kernel/sysctl.c:do_proc_dointvec` 和 `fs/read_write.c:default_llseek`。对应 RV64 参考配置必须启用 `CONFIG_SYSCTL` 与 `CONFIG_PROC_SYSCTL`；配置未启用导致双方 `ENOENT` 只能证明路径缺失，不能验收控制 ABI。

普通 `meminfo`、进程 `stat` 等生成文件由 OFD 持有一次 read epoch 的快照；sysctl 单整数却在任意非零读取偏移返回 EOF。即使首次只读取两字节，第二次 read 也不应接着输出剩余数字。seek 到零后必须观察当前全局配置。将两者共享 snapshot 路径会让短读和跨描述符更新产生可观察偏差，因此节点只为真正控制文件设置独立标记，生成普通文件继续原路径。

Linux 的 proc sysctl 在解析前完整复制整个 write iterator，包括后端不会消费的第二 token、超过解析窗口的后缀和非零偏移时会被忽略的输入。故 writev 第一段合法、第二段 fault 时不能先发布第一段数值。该协议由 files 层完整缓冲实现，VFS control 回调仅接收内核缓冲，scheduler setter 最终一次性验证 period/runtime 对并提交。读复制部分 fault 返回 `EFAULT`，文件偏移仍不变，这也不同于普通 proc 的成功前缀计数。

`do_proc_dointvec` 采用 base 0 数字、不接受加号，数字后的首个字符只能为空格、制表或换行；首尾跳过的空白集稍广。只解析一个整数并返回已消费前缀，不能把任意多 token 当成整体成功消费。超过一页的输入仍完整复制，解析窗口为 `PAGE_SIZE-1`，返回长度沿用固定 Linux 的剩余量计算。配置边界由 scheduler 验证，不在 proc 根据测试输入特判。

另一个独立差异是 seek：普通 proc snapshot 模拟 seq_file 的 `SEEK_END=-EINVAL`，而 sysctl 使用 `default_llseek`。新测试先在旧 controls snapshot 观察到四项 RED：SEEK_END 错误、SEEK_DATA/HOLE 错误，以及负 SEEK_HOLE 的 EOF 结果；修正仅作用于 controls。宽度为 32 位的配置解析、非零写忽略、用户 fault 与只读挂载由 `tests/diff-abi/rt_controls.c` 保持真实同 ELF 对照。
