# GPT 分区发现

RV `arch/riscv/root_boot.c` 与 LA `platform/loongarch_root.c` 在登记每块物理盘后调用
`kernel_block_scan_partitions()`。普通无分区文件系统与原有 MBR 四主分区路径保持兼容；
保护 MBR 转入 GPT 校验。两条启动路径都持有十五个稳定 child 对象并逐个清理。
这不是仅测试使用的 API：附加盘 GPT entry 2 对应 major 252、minor 18，用户可用
`mknod /dev/vdb2 b 252 18`，再以现有 `open`/块 I/O/`mount` 路径访问。
节点不会自动创建。根文件系统仍挂载第一块整盘；没有 GPT 根分区选择或 PARTUUID 解析。

## 语义和边界

* 只读发现，仅支持 512 字节逻辑扇区、GPT revision 1.0、纯保护 MBR。
  PMBR 必须有且仅有一个覆盖全盘（或 UINT32_MAX 饱和）的 0xee entry，其他 entry 全零。
  hybrid MBR、扩展分区、4Kn、loop 分区扫描、BLKRRPART、热重扫与自动修复不在范围内。
* 核验主/备用头的 header size、CRC32、当前位置、互指位置、磁盘 GUID、usable 范围，
  条目数组 CRC32、位置与长度；元数据须留在 usable 范围之外，至少预留 16 KiB。
  单份 CRC/几何损坏或 I/O 失败可采用另一有效副本；两份均有效但互相矛盾时拒绝。
  对已超支持范围的布局直接拒绝，不借另一副本悄悄截断。
* 数组最多 128 条，条目尺寸支持 128/256/512 字节；其他尺寸/数量返回 ENOTSUP。
  现有每盘十六 minor 的布局只给分区留 1..15，任何第 16 或之后的有效条目返回
  ENOSPC，而不是重编号或忽略。保留稀疏 entry 的原始编号。
* 有效条目需有非零 unique GUID，起止 LBA 合法，位于 usable 范围，不重叠且不重复 GUID。
  GPT 类型、名称、属性不作为自动挂载策略；不暴露 GUID/name 查询 API。
* 与 Linux 的宽容策略并不完全相同：例如保护 MBR 长度不符、混合 MBR、重叠与冲突副本
  在这里失败关闭，不能据正常镜像对照宣称所有损坏盘行为与 Linux 一致。

## 资源与工作量

发现持有父盘 request 引用和独占 transfer 锁，排除写入和容量变化；先校验所有内容，
再在 registry 锁内预检所有 child 状态、minor 冲突与父引用上限，一次发布整个表。
失败不留下半张表或 parent 引用。使用固定栈空间，无扫描期 heap 分配。
数组每份按扇区顺序只读一次，不为每条 entry 重读；只比较最多十五个有效范围。
直接 GPT API 的正常读取数为 `3 + 2 * ceil(entry_count * entry_size / 512)`；
启动分派多一次 PMBR 探测，GPT 锁内重新确认 PMBR，防止用锁外陈旧选择发布。
这些是结构与计数证据，不是吞吐或延迟收益测量。

## 验证

* `make test-gpt-host`：同一生产 parser 的 CRC/边界/尺寸/重叠/重复 GUID/槽位上限、
  主备 I/O 恢复、两份矛盾、minor 冲突与 busy child 原子失败/引用释放；断言读取次数。
  将同一有效 raw image 交给系统 util-linux **libblkid**，比较分区号/start/size。
  这是 Linux userspace 解析器对照，不是 Linux 内核的损坏盘差分测试。
* RV `BLOCK_TEST_TRANSPORT=modern BLOCK_TEST_CACHE=writeback make test-block-riscv`：
  真实 VirtIO 上自动分派、稀疏 entry、分区 I/O 翻译和主备 CRC 恢复。
* LA 对应 `make test-block-loongarch` 同样嵌入 GPT 功能检查；本次 NBD Unix socket
  fixture 无法启动，未声称该整条 IRQ 测试通过。RV/LA 生产内核构建通过。
* `python3 tests/userland/fat32-user.py --gpt --arch riscv --program <RV fat32 binary>`
  和 `--arch loongarch --program <LA fat32 binary> --qemu <LA QEMU>`：本次双架构均通过。
  Python/zlib 独立生成 66 MiB GPT 数据盘，保留 entry 1 空洞，在 entry 2 放真实 FAT32。
  整盘 ext4 启动生产内核，由用户程序 mknod/mount/read/write/卸载，最后核验 PID 1/heap
  释放、ext4 e2fsck 与抽取分区 fsck.fat。它验证真实用户入口，而非仅调用 discovery。
  本地编译的静态 musl FAT32 测试程序来自 `tests/userland/fat32.c`。
* 本地 LA 使用已有 QEMU 10.0.2，不具备项目 RTC toy 属性；临时 wrapper 仅去除
  block runner 的 `ls7a_rtc.toy-enabled=on`，未改生产 RTC 代码或宣称 RTC 验收。

参考：UEFI 2.10 §5 GUID Partition Table；Linux v6.12 `block/partitions/efi.c`。
没有执行宽泛性能基线、Docker 或修改 FLUSH/旧 IOzone 锁。

Host 另外通过原 MBR/partition/RMW/lifetime 检查（UP 与双 CPU host model）和
ASan/UBSan GPT 检查。容器 ptrace 环境不支持 LeakSanitizer，因此该次 sanitizer 运行
使用 `ASAN_OPTIONS=detect_leaks=0`，没有把它描述成泄漏检测通过。
