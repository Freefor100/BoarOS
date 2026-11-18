# 数据路径实验工具

存储与网络的当前机制分别见[VFS/ext4](../modules/vfs-ext4.md)、[块设备](../modules/riscv-virtio-block.md)和[网络模块](../modules/kernel-network.md)。本页说明仍在使用的固定工作量工具与比较口径；旧版本的候选矩阵和逐轮数据保留在 Git 历史。

## 存储

`tests/cost/io-budget-experiment.py` 使用 `tests/workloads/io/data-path.c` 和 `storage-path.c`，覆盖追加、覆写、冷热读取、并行追加、范围修改、同步写、writev、mmap 写和同步期间的独立读取。当前 4 KiB 页配置的预读与写回默认参数为 RA16/WB32，分别对应 64 KiB 和 128 KiB；实际设备请求仍由连续范围及 SG 限制决定。

```sh
python3 -B tests/cost/io-budget-experiment.py profiles
python3 -B tests/cost/io-budget-experiment.py --help
python3 -B tests/cost/io-budget-experiment.py run --help
```

case 格式为 `backend:operation:completion:files:bytes_per_file:request_bytes`。普通完成条件区分 cache、fsync 和 fdatasync；仅 ext4 支持预置未读数据的 cold-read，tmpfs read 为驻留内存。四文件轮转与四个并发执行者是不同工作量；mixed-read 由一个预先弄脏数据的写者、两个独立冷读者和一个 open/close 执行者组成，准备不计入窗口。

正式比较显式传入 `--baseline` 与 `--variant label=kernel`；`--program` 可复用同一个 ELF。runner 记录每文件实际字节、校验和、同步、请求时间、进程 CPU、亲和性与资源归还，检查正常退出及文件系统。双 CPU 与主机线程绑定依照实际 USER_SMP 配置设置。

### 目录枚举

`tests/workloads/fs/getdents_batch.c` 接受 `DIRECTORY ENTRIES BUFFER_BYTES ROUNDS [prepare]`。
`prepare` 仅用于空目录中创建固定编号文件并同步，准备与一遍预热均不计时；计时窗口仅包含
rewind、getdents64 与名称唯一性核对。输出实际 entries、返回 bytes、syscall 次数、wall_ns
和 process cpu_ns。这明确是暖目录，不得当作冷盘结果。示例输入为 4096 项、4096/32768 字节
buffer、相同 rounds；32 字节 buffer 每次只容纳一项，是不能摊薄 iterator/usercopy 的对照。
同一 ELF、目录镜像、页大小与总枚举项数分别运行基线和候选，比较 entries/s 与 CPU ns/entry。
另行诊断时可统计 `ext4_dir_iterator_init`、inode-ref 获取及 `kernel_copy_to_user` 次数；正式计时
关闭诊断。并发写者的创建/删除延迟应单独观察，不能以枚举加速掩盖批次锁延长的代价。

IOzone 以文件内容读写为负载，不覆盖目录枚举，不能拿其分数证明 getdents 改进。它在这里仅
作为无关数据路径的回退对照：保持原 IOzone ELF、文件/record 大小、并发进程数与 fsync 选项
一致，读写/重读/重写分别报告，不将写入返回与持久化完成合并，也不并行运行两个版本。

## 网络

`tests/cost/network-budget-experiment.py` 使用 `tests/workloads/network/tcp-budget.c`，覆盖 loopback/TAP、阻塞/非阻塞、收发、bulk、请求响应与混合控制连接。普通接口的默认预算为 `w8-p4-m2`：8 MSS 窗口与发送缓冲、4 倍协议池、2 倍协议堆；回环连接另用 8192 字节 MSS 与 32768 字节窗口。PCB 容量不随候选增加。

```sh
python3 -B tests/cost/network-budget-experiment.py profiles
python3 -B tests/cost/network-budget-experiment.py --help
python3 -B tests/cost/network-budget-experiment.py run --help
```

`build --profiles ...` 构建独立参数候选，并把源码、编译器、宏与观测状态绑定到内核。`run --variant label=kernel` 只运行显式指定的版本；需要参考时添加 `--baseline` 或 `--linux-kernel`。每项 case、版本和重复使用独立启动与磁盘副本；TAP 使用独立网络命名空间，netem 延迟按实际回显 RTT 解释。接收端字节与校验和完成才算传输完成，混合负载还检查控制连接在 bulk 期间实际前进。

扩大池缓解全局协议资源接纳压力，不能消除单连接的小窗口／RTT 上限，也不能替代 TCP 连接归属、协议执行资格或设备完成处理。静态数组和运行期管理内存分别统计，不能用相近的堆峰值掩盖更大的静态池。

## 比较与诊断

`smoke` 只检查功能；正式 `run` 至少三个独立启动，COST 关闭，且与其他测量串行。`--observe` 使用独立诊断启动，检查当前完整快照；开销由匹配的 ON/OFF 比较获得。

存储的 cache 完成、durable 同步和最终 checkpoint／卸载是不同边界。请求占比不是时间占比，嵌套 IRQ／锁／等待计时不能相加。每文件完成时间和有限传输的速率分布描述本次工作，不作为调度饥饿上界。剩余串行范围与待处理回退由[开发路线](../goals.md)统一维护。
