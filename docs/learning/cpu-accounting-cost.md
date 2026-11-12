# CPU 记账与固定工作量测量

当前记账机制见[资源记账模块](../modules/kernel-accounting.md)，等待、PI 与运行队列的共享状态见[调度模块](../modules/kernel-scheduler.md)。CPU 时间、墙钟和信号递送次数分别记录；累计任务时间可以重叠，不能相加解释整次运行耗时。

## 可复用消费者

`tests/cost/accounting/cost.c` 与 `cost.py` 使用同一 ELF 比较指定内核。默认套件执行固定计算、tmpfs 小 I/O、SQLite 内存查询和同步事务；`--timer-suite` 执行 getpid、计算、tmpfs I/O、八线程和八进程的固定总工作量，分别运行关闭、已设置和周期性 VIRTUAL/PROF 定时器。

```sh
make build-accounting-cost
python3 -B tests/cost/accounting/cost.py --help
```

运行时显式提供 `--arch`、`--kernel label=path`、`--program` 和新的 `--output`。RV 另提供 `--firmware`，默认 SQLite 套件提供 `--sqlite`；定时器套件使用 `--timer-suite`，并用 `--timer-modes` 选择本次状态。多 CPU 定时器测量要求真实 USER_SMP 内核；`--host-vcpu-cpus` 和 `--host-io-cpu` 在客户机开始前固定实际线程亲和性。

执行器轮换版本与状态，核对固定校验和、实际 CPU 集合、并行重叠和信号行为，最后检查正常退出、资源归还和只读 fsck。周期状态必须观察到递送，未设置和仅设置状态检查各自信号语义。

## 比较口径

新比较必须保持 ELF、输入、总工作量、同步完成条件和退出检查一致。正式时间关闭 COST，诊断另跑；实际宿主频率与抢占、TCG 翻译和运行位置也会影响 QEMU 墙钟。计数下降说明重复工作减少，应用收益由相同条件下的耗时和吞吐判断。
