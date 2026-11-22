# 文件时间戳、所有权与持久化语义

在类 Unix 文件系统中，每个索引节点（`inode`）维护三个核心时间戳：**访问时间（`atime`）**、**内容修改时间（`mtime`）** 与 **元数据状态变更时间（`ctime`）**，以及表示文件归属的用户标识（User ID，UID）和组标识（Group ID，GID）。看似简单的几个字段，实际牵涉三个棘手的工程问题：

1. **性能与语义平衡**：如果每次 `read` 都立即开启文件系统日志事务去写盘更新 `atime`，只读负载也会退化为密集随机写。
2. **精确的请求触发边界**：零长度读写、越界写入（`-EFBIG`）、首字节用户态地址错误（`-EFAULT`）和同长度截断（`ftruncate`）分别该不该更新时间？顺序错一步就会违背 Linux 应用二进制接口（Application Binary Interface，ABI）契约。
3. **磁盘格式边界与崩溃一致性**：传统 128 字节 ext4 `inode` 在 2038 年后会溢出，扩展 `inode` 需要正确编码纪元（epoch）与纳秒；修改文件属主（`chown`）涉及双 16 位磁盘字段与共享扩展属性（Extended Attribute，EA）块的写时复制（Copy-on-Write，CoW）回滚。

本文依据 `fs/vfs.c`、`fs/files/metadata.c`、`fs/files/io.c` 与 `fs/ext4_backend.c` 的实际代码，系统说明 BoarOS 的文件时间戳、所有权与持久化实现。VFS 挂载策略与事务协作详见 [VFS 与 ext4 文件系统](../modules/vfs-ext4.md)。

---

## 一、运行时节点所有权与 `relatime` 延迟写回

### 1.1 时钟源与活节点（Live Inode）

虚拟文件系统（Virtual File System，VFS）在 ext4 后端通过 `vfs_realtime()` 读取内核实时时钟（`kernel_time_realtime_ns()`）。当系统时钟尚未初始化（例如独立宿主测试桩）时，该函数返回不可用，保留磁盘原有时间戳；正常启动后以有符号 64 位秒数和纳秒精度表示时间。

普通文件首次打开时，`fs/ext4_backend.c` 从磁盘 `inode` 解码 `atime`、`mtime`、`ctime` 到共享的内存活节点 `struct lwext4_node`，之后所有指向同一 `inode` 的硬链接别名、以及已被 `unlink` 但仍有打开描述符引用的文件，全部读写这份统一的运行时时间，由短临界区 `TIMESTAMP_SCOPE`（多核为自旋锁 `adapter->timestamp_lock`，单核为中断保护）协调并发可见性。

### 1.2 `relatime` 策略与 100 ms 批量落盘

Linux 挂载默认采用 **`relatime`（相对访问时间）** 策略，避免每次读盘都产生元数据写放大：只有当满足以下任一条件时，读取操作才更新 `atime`：

- 当前 `atime <= mtime` 或 `atime <= ctime`（文件内容或元数据在最后一次访问之后发生过变化）；
- 距离上次 `atime` 更新已满 24 小时（`now.seconds - atime.seconds >= 86400`）。

BoarOS 对热读路径做了两层优化：

1. **无锁快速跳过（`relatime_due`）**：每当 `atime`/`mtime`/`ctime` 变化时，`publish_relatime_due()` 会原子发布下一次需要更新 `atime` 的到期秒数（若 `mtime >= atime` 或 `ctime >= atime` 则设为 `INT64_MIN` 表示立刻到期，否则设为 `atime.seconds + 86400`）。读路径无需加锁即可快速判断当前是否无需刷新 `atime`。
2. **延迟合并写回**：普通 `read`/`write` 绝不为每次访问单独开启日志事务，而是通过 `time_dirty_locked()` 更新内存中的 `node->times[i]`，递增字段版本号 `node->time_version[i]`，标记 `dirty_fields`，并将节点挂入挂载点的 `dirty_times` 链表。首次有节点变脏时，设置 **100 ms**（`100,000,000` ns）的批量刷新截止时间 `adapter->times_deadline`，交由挂载点的后台日志/写回线程统一合批写入。

> 挂载选项支持 `relatime`、`noatime`、`nodiratime` 与 `strictatime`（按 VFS 层挂载策略管理；注意内核启动参数不把 `rootflags=relatime` 直接透传给 ext4 内部解析器，而是由 VFS 挂载标志统一控制）。带有 `EXT4_INODE_FLAG_NOATIME` 标志的 `inode` 或使用 `O_NOATIME` 打开的描述符同样跳过 `atime` 更新。

---

## 二、各类系统调用的精确触发边界

内核在文件层与 VFS 层严格对齐 Linux 的错误检查顺序与时间戳更新时机：

| 操作类型 | 触发条件与检查顺序 | 更新字段 |
| :--- | :--- | :--- |
| **`read` / `pread`** | 先校验 FD、读权限及用户缓冲区地址范围（`pread` 在 `MAX_RW_COUNT` 截断与文件尾判断前校验完整用户范围，包含指向非法高地址的零长度请求，`-EFAULT` 不更新时间）。零长度请求跳过；非零读取即使命中页缓存、遇到文件尾（EOF）或部分拷贝，仍按 `relatime` 判定更新。 | `atime`（受 `relatime` / `noatime` 约束） |
| **`write` / `writev`** | 先校验 FD、写权限与按实际写入/追加偏移计算的 `inode maxbytes` 上限（超限返回 `-EFBIG` 且不更新时间）。随后对非零普通文件写入在**拷贝用户数据之前**更新时间戳——因此即便首字节用户拷贝触发 `-EFAULT`，`mtime`/`ctime` 也已更新；但返回值与打开文件偏移严格只计后端实际提交的字节数。 | `mtime`、`ctime` |
| **创建文件/目录** | 创建新 `inode` 时直接初始化三项时间戳；初始权限位通过同一 `inode` 句柄在原事务内设置，不重新解析路径，也不额外重采 `ctime`（与普通 `chmod` 更新 `ctime` 严格区分）。创建或移除目录项成功时更新父目录时间；`unlink` 同步更新子 `inode` 的 `ctime`。 | 新建：`atime`/`mtime`/`ctime`；父目录：`mtime`/`ctime` |
| **`truncate` / `ftruncate`** | 在活 `inode` 尺寸修改路径后更新，**即使目标长度与原长度相同**也更新时间戳。 | `mtime`、`ctime` |

### 显式设置：`utimensat` 与 `futimens`

`fs/files/metadata.c` 中的 `kernel_files_utimensat()` 严格遵守以下判定优先级：

1. **先拷入 `times[2]`**：若用户提供非空 `user_times`，先从用户态拷贝两个时间结构（非法指针直接返回 `-EFAULT`）；若两项纳秒字段均为 `UTIME_OMIT`（忽略不改），立刻返回 `0`，不做路径解析。
2. **区分“描述符形式”与“路径形式”**：
   - **描述符形式**（`user_path == NULL` 且 `dirfd != AT_FDCWD`，musl 的 `futimens()` 即采用此形式）：要求 `flags` 必须为 `0`（否则 `-EINVAL`），禁止对 `O_PATH` 描述符操作（`-EBADF`），且直接按底层 `inode` 修改，不受文件已被 `unlink` 的影响。
   - **路径形式**（提供路径字符串，或空字符串配合 `AT_EMPTY_PATH`）：仅允许 `AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH` 标志，先完成路径解析再检查纳秒合法性与只读挂载（`-EROFS`）。
3. **单次采样一致性**：后端只采样一次实时时钟，保证同时指定 `UTIME_NOW` 的字段与自动刷新的 `ctime` 严格相等；指定 `UTIME_OMIT` 的字段与父目录时间戳保持不变。

> 在编写 ABI 对照测试时需留意 C 标准库封装差异：例如 musl 1.2.5 的 `statfs`/`fstatfs` 包装会在发起系统调用前先在用户态清零输出缓冲区，因此测试非法输出指针必须使用原始系统调用（raw syscall），避免把用户态自身触发的段错误误判为内核错误码。

---

## 三、ext4 磁盘时间编码与 2038/2106 边界

ext4 支持两种 `inode` 尺寸，决定了时间戳的表示范围与精度：

| `inode` 格式 | 磁盘存储字段 | 支持秒数范围与纳秒精度 |
| :--- | :--- | :--- |
| **传统 128 字节 `inode`** | 仅 32 位有符号秒字段 | `[-2147483648, 2147483647]`（1901–2038 年），纳秒固定为 `0` |
| **扩展 `inode`（256 字节等）** | 32 位有符号基数 + `atime_extra`/`mtime_extra`/`ctime_extra`（2 位纪元 + 30 位纳秒） | `[-2147483648, 15032385535]`（覆盖至 2446 年），支持纳秒精度 |

`fs/ext4_backend.c` 中的 `decode_inode_time()` 与 `normalize_time()` 同时核对磁盘超级块的 `inode_size` 以及当前 `inode` 自身的 `extra_isize`：

- 只有当对应 `*_extra` 字段完整落在 `extra_isize` 范围内时，才启用扩展编解码，绝不把不存在的扩展字段越界写进下一个 `inode`。
- 写入超过支持范围的时间值时，遵循 Linux `timestamp_truncate()` 规则：秒数截断至 `[INT32_MIN, maximum]`，且达到或越过边界端点时**清零纳秒**，同时确保 2038 年之后的无符号低 32 位配合纪元位正确解码，不被误解为负数。

---

## 四、文件所有权（`chown`）与共享扩展属性块

文件 `inode` 上记录的 `uid`/`gid` 是对象自身元数据，与当前进程身份相互独立。匿名管道的属主元数据归共享管道对象持有，命名管道（FIFO）归 VFS `inode` 持有，两者互不混淆。

BoarOS 的 `fchown`/`fchownat`（`fs/files/metadata.c`）与权限/模式辅助函数（`fs/vfs.c` 中的 `kernel_vfs_chown_allowed()`、`kernel_vfs_chown_mode()`）遵循以下规则：

1. **路径与空路径区分**：与 `utimensat` 不同，`fchownat` 传入 `NULL` 路径指针返回 `-EFAULT`，传入空字符串配合 `AT_EMPTY_PATH` 才按 `dirfd` 指向的打开文件描述（包括无路径匿名管道与已 `unlink` 的 `inode`）修改属主。
2. **时间戳与 set-ID 位处理**：`chown` 更新 `ctime`，不改变 `atime`/`mtime`；针对非目录文件，修改属主后清除 `S_ISUID` 位，且仅在组执行位（`S_IXGRP`）置位时清除 `S_ISGID` 位；目录则保留 set-ID 位，并在 `setgid` 目录下创建新对象时继承目录 GID。
3. **ext4 双 16 位 UID/GID 与安全能力属性**：
   - ext4 将 32 位 UID 与 GID 拆分为低 16 位和高 16 位两组磁盘字段存储，修改大编号 ID（如 `70001:80002`）必须同步更新高 16 位，避免截断成错误属主。
   - 非目录执行 `chown` 还需移除关联的 `security.capability` 扩展属性，并与 UID/GID、`mode`、`ctime` 共享同一操作级撤销（undo）记录。
   - 扩展属性既可能内联在扩展 `inode` 中，也可能存放在多个 `inode` 共享的外部扩展属性（EA）块中。修改共享 EA 块前必须先复制出私有副本，并同步维护旧块引用计数、新块校验和与 `inode` 占用扇区数；若操作中途失败，仅回滚本次修改，绝不破坏其他 `inode` 共享的属性块。删除 `inode` 时同样只递减自身对 EA 块的引用，仅在最后一份引用归零时释放物理块，防止位图误标空闲引发静默数据损坏。

---

## 五、失败语义、版本交错与同步边界

为了在延迟写回与显式修改并发交错时不丢更新、不冒充持久化，BoarOS 在 `fs/ext4_backend.c` 中采用**暂存快照 + 字段版本比对**协议：

- **写回延迟采样**：数据页写回直到真正获得事务准入资格后，才通过 `time_snapshot()` 捕获当前内存时间戳，避免排队等待期间发生的较新命名空间修改被陈旧快照覆盖。
- **暂存失败不污染内存态**：显式设置时间或命名空间操作在事务准备阶段记录 `struct lwext4_staged_inode`。若准备失败（如内存不足）或事务回滚，暂存时间直接丢弃，内存与磁盘保持原值；事务提交成功后，仅清除那些**版本号未在期间继续递增**的脏字段标记，确保并发新到来的 `atime` 更新得以保留。
- **区分读错误与同步边界**：
  - 已知挂载错误在写路径拷贝用户数据前即刻返回；后续编码、日志提交或刷盘错误由节点、日志与挂载点持久保留，并在同步调用中上报。
  - 读路径上的 `atime` 元数据错误按 Linux 语义绝不覆盖 `read()` 本身的成功返回值。
  - 完整 `fsync()` 会先将脏时间戳编码进事务，再捕获持久化目标提交点；`fdatasync()` 保持“仅同步恢复数据所需元数据”的语义，纯时间戳脏位不会单独推进数据同步目标，但随数据页同事务编码的时间戳会一并落盘。

---

## 六、验证矩阵

时间戳与所有权语义通过宿主级磁盘格式检验、内核日志事务测试及双架构 Linux 差分测试共同覆盖：

```sh
# 真实 VFS / 日志线程交错测试（含无脏页 fsync、关闭后后台保存、无日志镜像后台写盘）
make test-journal-group-riscv

# 聚焦写进度与同步失败区分测试
make test-files-partial-write-riscv

# 宿主 128/256 字节 inode 时间边界（relatime/负纪元/2038/2106/e2fsck -fn）与属主/EA 测试
make test-lwext4-host test-lwext4-metadata-host
sh tests/host/lwext4-metadata.sh ownership

# 用户态与同 ELF Linux 差分回归（含 utimensat、write maxbytes、pread 越界等全字段时间区间比对）
make test-userland-riscv
make test-diff-abi-riscv
```

在真实用户态差分对比中，测试不把时间戳归零比对，而是保留全部时间字段及操作前后 `CLOCK_REALTIME`（并在区间起始前留出 50 ms 窗口以兼容 Linux `current_time()` 粗粒度时钟特性），严格校验变更字段落入合法区间且相等/抑制关系与 Linux 一致；磁盘镜像测试最后均经独立 `e2fsck -fn` 校验格式完整性。
