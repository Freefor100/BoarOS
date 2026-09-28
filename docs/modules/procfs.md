# procfs 模块

`fs/procfs.c` 是可动态创建的 VFS 实例。当前提供真实物理页分配器读数的 `meminfo`、单 hart 运行/idle 计时的 `uptime`、按活进程枚举的数字目录、`/proc/self` 与进程的 `exe/cwd/root` 对象链接，以及 `/proc/self/mounts` 与 `/proc/mounts`。`stat/status` 和 `fd/` 尚未接入；不能把这批入口当作完整 procfs。

`mount(2)` 接受类型 `proc`、普通挂载、`MS_RDONLY` 与 `MS_SILENT`；源字符串只作用户地址验证，不选择设备。用户态负责创建挂载点。其他挂载模式显式拒绝。`umount2(2)` 只接受 flags 0，要求名称解析到 proc 实例根且无 cwd、文件、子挂载或其他路径引用。挂载实例持有根及覆盖路径，失败创建或发布不会改变原树；成功卸载先摘树、释放根，最后销毁无 I/O 的 proc 实例。PID 1 及后代全部退出后，根盘清理由最深子挂载开始迭代拆除 proc，不依赖用户态逐个卸载。当前所有任务共享一棵挂载树。

生成式普通文件不进入 ext4 页缓存。后端 `snapshot` 返回由 OFD 持有的堆缓冲；第一次非空读取形成快照，短读、readv 和 pread 使用它，seek 到零后释放并在下一次读取重新取值，`SEEK_END` 按固定 Linux 的 seq_file 语义返回 `EINVAL`。用户复制时不持有后端或 inode 锁；只推进已经复制的字节。OFD 末引用释放快照，mount 忙判断覆盖文件及目录引用。`fstat` 中的 size 为零是生成文件的元数据语义，不能由它推断 read EOF。

`MemTotal` 和 `MemFree` 单位为 KiB，来源分别是 `physical_page_total()` 和 `physical_page_available()`；它们描述 BoarOS 管理的物理页，并不冒充尚未统计的缓存、交换或可回收页分类。固定 Linux 的 root 可以用写模式打开 `meminfo`，但写入返回 `EIO`；该行为与内容不可修改一致。未知目录项返回 `ENOENT`。首批 U-mode 对照见 `tests/diff-abi/proc.c`；内部挂载树测试见 `tests/riscv/vfs_main.c`。

PID 数字目录的 inode 编入单调分配代次；进程退出后旧目录对象不能通过复用的 PID 转向新任务。`/proc/self` 的链接文本在每次调用时读取当前任务 TGID，不把首个访问者缓存为全局目标。`exe` 来自主程序 MM 单独持有的 OFD 引用，动态解释器不取代它；fork 的子 MM 取得独立引用，共享 MM 则共享原 owner。`cwd/root` 来自目标任务当前 fs context。VFS 跟随这三个链接时直接取得活路径引用，避免对展示字符串重新解析；readlink 仅生成展示文本，已删除对象标注 `(deleted)`。任务查询在单 hart 短关中断区内取得快照或路径引用，不钉住任务栈。生成文本和对象链接的聚焦对照为 `make test-diff-abi-riscv`；未实现链接不返回伪造内容。

`/proc/mounts` 的链接文本为 `self/mounts`。进程 `mounts` 的第一次非空读取在短关中断区捕获整棵共享挂载树，并分别钉住挂载根与被覆盖路径；随后无锁格式化并由 OFD 持有本次文本。根 ext4 行的来源名为内部 `rootfs`，只表达本内核的根盘身份，不伪称 `/dev` 节点；proc 行的来源和类型均为 `proc`。输出包含实际挂载点、后端类型、实例的 `ro/rw` 状态和 `0 0` 字段，路径中的空白、反斜杠及 `#` 用八进制转义。卸载期间的引用会使普通卸载返回 `EBUSY`，快照交付后立即解除临时引用。`make test-diff-abi-riscv` 在固定 Linux 对照链接文本、打开能力和真实 proc 行，当前为 617 条；嵌套挂载与并发卸载还需要专门的真实用户态用例。
