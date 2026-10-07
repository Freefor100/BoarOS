# 共用 VirtIO transport 与 split queue

入口为`include/kernel/virtio_transport.h`、`virtio_split_queue.h`和
`drivers/virtio/{transport,split_queue,mmio,pci}.c`。block、RNG、net均已迁入；三个设备的业务策略与owner仍各自持有。LA真实
PCI RNG/net和退出回收均有独立验收，完整平台/程序矩阵仍在推进。

## 分层与硬件契约

transport拥有设备身份/版本、feature/status、队列配置/通知、配置读取、IRQ确认
和reset确认。ops按值复制，context仍借用持久平台owner；RV物理启动别名中的
回调逐项从实际PC构造，不能加载高地址函数常量表。架构屏障继续构建期绑定。
设备核心不访问MMIO/PCI偏移；PCI的ISR读取即清除，MMIO先读pending再W1C。

`begin`先确认reset，现代设备必须协商VERSION_1并回显FEATURES_OK；必需feature
缺失返回UNSUPPORTED，设备拒绝已写入的FEATURES_OK返回DEVICE。legacy仅
使用低32位feature。`start`完成DMA发布屏障后设置DRIVER_OK并检查失败/reset状态。
`queue`拒绝已启动设备、已启用现代queue、容量不足、地址溢出或不合法legacy页布局。
MMIO/PCI由同一个核心配置，传统guest page/PFN与现代三组地址仍遵循各自协议。

配置读取按设备区域长度和1/2/4字节自然访问宽度检查；设备核心拥有一致性重读
策略。PCI标准common/notify/ISR必须存在，device config可省略：RNG没有配置区，
block要求8字节，其他消费者同样明确要求其有效字段。每个queue分别保存已检查的
notify位置，不能将queue0的notify地址用于所有queue。非法外部notify范围返回
UNSUPPORTED，不能作为内核状态损坏fatal。

PCI平台持有function/BAR、映射和共享INTx来源。初始化在BAR分配后确认旧DMA停止，
随后才开启bus-master。reset不能确认时保留function/BAR；销毁必须在设备业务
owner收口、IRQ摘除之后再次确认reset并恢复原BAR/command。不会扫描固定槽号。
block包装层记录core owner是否已消费，根清理也纳入尚未构造core的PCI半成品。
最后PCI reset失败后仅重试剩余BAR owner；已经释放的队列不能再次传给core销毁。

## descriptor 与业务 owner

当前split queue最多32项；descriptor、available/used idx及used元素按协议自然
对齐，idx以单次半字读写。调用方用单CPU IRQ排他串行化发布、收割和reset；这不
提供SMP锁。队列内存由设备核心分配，框架只借用，不分配或释放物理页。

调用方只能构造空闲descriptor。`publish`检查NEXT链范围/循环、flags、DMA地址
溢出、链间descriptor冲突及indirect协商/表地址长度边界；在推进available idx之前登记
head/token/链mask和完成长度边界。token是业务owner的不透明身份，不被框架解引用。
发布后设备对象、transport和队列地址必须保持稳定。

`take`先读used idx并执行DMA屏障，检查ring容量、已发布head与长度边界。每次
合法完成返回token并归还descriptor资格；它不释放业务buffer、RX loan、pbuf或
调用方的块请求内存。已收割的合法完成不因后续重复/非法项改变结果；错误保留
未收割token、used/consumed/head/length快照，设备核心决定停止与错误传播。

`reset`只有在本队列绑定的transport确认quiescent后才能撤销剩余token和重置
ring索引；另一个设备的reset确认不能代用。业务buffer继续属于设备核心，完成
或DMA停止只是它归还这些buffer的前提。参数错误、不支持、资源不足、设备错误、
状态错误及empty/full分别报告；等待期限与TIMEOUT仍由业务核心拥有。

## block 接入与边界

block保留八个请求槽、预留/发布/完成/调用方消费状态、batch/RMW/flush、I/O
context、等待、30秒期限和统计。legacy/modern ring布局和请求buffer布局保持，
配置capacity经transport读取。读完成最多数据长度+status，写/flush完成只允许
status长度；非法长度在reset前诊断为`used-length`，零长度仍为`used-element`。
块路径仍限定固定QEMU同步reset：若不能确认，不能返回借用的调用方DMA，保留
原fatal契约；随机/网络的持久业务owner另有显式错误与重试策略，不能由框架统一吞掉。

## 验证

```sh
make test-virtio-framework-host
make test-virtio-block-host test-pci-host
make test-block-riscv test-block-loongarch
make test-io-sleep-riscv test-root-loongarch
make test-pci-reset-owner-loongarch test-root-io-loongarch
make test-stack-usage test-stack-usage-la
make test-sqlite-recovery-matrix-riscv test-sqlite-wal-recovery-matrix-riscv
```

host验证feature/reset失败、三个adapter的访问宽度/notify/BAR、token与descriptor
分离、错误设备reset拒绝、链冲突、indirect、反序/重复完成、非法长度/ID和65544
次ring绕回。实际block模型继续检查reset前诊断、batch取消与borrowed DMA；RV
交叉编译检查自然半字访问。host边界模型不能替代真实DMA/IRQ或原程序验收。

真实块门禁包含legacy/modern MMIO与modern PCI、readonly/batch/flush、LA共享INTx
及两种RAM的页/堆/任务栈/BAR回收；真实可睡眠I/O门禁覆盖两种传输×两种缓存
模式、暂扣响应时计算进展、八槽部分补发、错误排空、取消和超时reset。
真实PCI上的确认失败注入另验证初始化半成品、core消费后的BAR重试及根启动
EIO/未发布PID1/资源基线；这是软件边界注入，固定QEMU自身仍提供同步reset。
扩大RV架构、真实用户态、五形态glibc2.44、1366条ABI以及SQLite DELETE/WAL
完整NBD恢复矩阵通过；RNG/net迁移和全计划最终回归仍需独立完成。

固定依据为`references/linux` commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`的`include/uapi/linux/virtio_ring.h`，
`references/qemu` v11.1.0 commit`84f07211cc5b4fc6a371559bf8a5de4fb068e648`的
`hw/virtio/virtio{,-mmio,-pci}.c`、`virtio-rng.c`和`hw/block/virtio-blk.c`。

net通过同一套typed配置/IRQ/queue API，不再访问MMIO/PCI偏移。MAC和link一致性
仍由net核心读取、重试并拒绝非法值。IRQ确认与descriptor收割不归还RX loan或
SG pbuf；`quiesce`只停止DMA、摘IRQ和撤token，业务owner由网络层归还，随后
core释放DMA页，平台释放BAR。完整细节与实际MMIO/PCI矩阵见[net模块](riscv-virtio-net.md)。

net的init输入是新的零初始化device对象；当前platform每次持有一个完整的冷启动
owner生命周期。成功stop归还全部loan/TX/DMA/IRQ owner后，调用方可清零并重新
初始化存储；failed reset保留的对象只能继续完成stop，不能清零或重新init覆盖owner。
当前没有未清零对象的热插拔/原位重启接口，这个存储前提不封锁后续生命周期扩展。
