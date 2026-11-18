# GPT 分区发现

RV `arch/riscv/root_boot.c` 与 LA `platform/loongarch_root.c` 在登记每块物理盘后调用
`kernel_block_scan_partitions()`。普通无分区文件系统与原有 MBR 四主分区路径保持兼容。
保护 MBR 转入 GPT 校验。两条启动路径都持有十五个固定的分区对象并逐个清理。
第二块磁盘的 GPT 第 2 项对应主设备号 252、次设备号 18，可以用
`mknod /dev/vdb2 b 252 18`，再以现有 `open`/块 I/O/`mount` 路径访问。
节点不会自动创建。根文件系统仍挂载第一块整盘；没有 GPT 根分区选择或 PARTUUID 解析。

## 语义和边界

* 只读发现，仅支持 512 字节逻辑扇区、GPT revision 1.0、纯保护 MBR。
  PMBR 必须有且仅有一个覆盖全盘（或 UINT32_MAX 饱和）的 0xee entry，其他 entry 全零。
  hybrid MBR、扩展分区、4Kn、loop 分区扫描、BLKRRPART、热重扫与自动修复不在范围内。
* 核验主/备用头的 header size、CRC32、当前位置、互指位置、磁盘 GUID、usable 范围，
  条目数组 CRC32、位置与长度；元数据须留在 usable 范围之外，至少预留 16 KiB。
  单份 CRC/几何损坏或 I/O 失败可采用另一有效副本；两份均有效但互相矛盾时拒绝。
  若头部声明的布局超过实现上限，拒绝整张分区表。
* 数组最多 128 条，条目尺寸支持 128/256/512 字节；其他尺寸/数量返回 ENOTSUP。
  现有每盘十六 minor 的布局只给分区留 1..15，数组 CRC 校验通过后，第 16 或之后有占用项时返回
  ENOSPC。保留稀疏分区表的原始编号。
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

## 验证

- `make test-gpt-host` 检查 GPT 校验、主备恢复和原子发布，并用 libblkid 对照同一镜像的分区号、起始扇区和长度。
- `BLOCK_TEST_TRANSPORT=modern BLOCK_TEST_CACHE=writeback make test-block-riscv` 和
  `make test-block-loongarch` 在 VirtIO 块设备上检查分区偏移、越界和主备恢复。
- `tests/userland/fat32-user.py --gpt` 使用整盘 ext4 启动和 GPT 数据盘。分别传入
  RV64 或 LA64 的生产内核和 `tests/userland/fat32.c` 的静态 ELF 后，运行
  分区建节点、挂载、读写和卸载，并检查两个文件系统的一致性。

[GPT 磁盘格式（UEFI §5）](https://uefi.org/specs/UEFI/2.10_A/05_GUID_Partition_Table_Format.html)。
