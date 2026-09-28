# procfs 模块

`fs/procfs.c` 是可动态创建的 VFS 实例，当前只发布根目录和由真实物理页分配器读数生成的 `meminfo`。进程目录、对象链接、`uptime` 和挂载列表尚未接入；不能把这个阶段的 mount 成功当作完整 procfs。

`mount(2)` 接受类型 `proc`、普通挂载、`MS_RDONLY` 与 `MS_SILENT`；源字符串只作用户地址验证，不选择设备。用户态负责创建挂载点。其他挂载模式显式拒绝。`umount2(2)` 只接受 flags 0，要求名称解析到 proc 实例根且无 cwd、文件、子挂载或其他路径引用。挂载实例持有根及覆盖路径，失败创建或发布不会改变原树；成功卸载先摘树、释放根，最后销毁无 I/O 的 proc 实例。PID 1 及后代全部退出后，根盘清理由最深子挂载开始迭代拆除 proc，不依赖用户态逐个卸载。当前所有任务共享一棵挂载树。

生成式普通文件不进入 ext4 页缓存。后端 `snapshot` 返回由 OFD 持有的堆缓冲；第一次非空读取形成快照，短读、readv 和 pread 使用它，seek 到零后释放并在下一次读取重新取值，`SEEK_END` 按固定 Linux 的 seq_file 语义返回 `EINVAL`。用户复制时不持有后端或 inode 锁；只推进已经复制的字节。OFD 末引用释放快照，mount 忙判断覆盖文件及目录引用。`fstat` 中的 size 为零是生成文件的元数据语义，不能由它推断 read EOF。

`MemTotal` 和 `MemFree` 单位为 KiB，来源分别是 `physical_page_total()` 和 `physical_page_available()`；它们描述 BoarOS 管理的物理页，并不冒充尚未统计的缓存、交换或可回收页分类。固定 Linux 的 root 可以用写模式打开 `meminfo`，但写入返回 `EIO`；该行为与内容不可修改一致。未知目录项返回 `ENOENT`。首批 U-mode 对照见 `tests/diff-abi/proc.c`；内部挂载树测试见 `tests/riscv/vfs_main.c`。
