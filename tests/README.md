# 测试目录

当前入口按执行环境组织，测试程序按用途组织。一个文件可能是多个用例共用的夹具、后端或运行器，并不等于一个测例。日常内核检查仍通过 `make test-*` 执行；具体分组见 `tests/ci/suites.json`。

| 目录 | 内容 |
|---|---|
| `host/` | 宿主直接检查生产代码；C 夹具和相应 `.sh`／`.py` 入口放在一起 |
| `riscv/`、`loongarch/` | 架构与平台的内核契约、启动检查和 QEMU 执行器 |
| `common/` | 两架构共用的原生契约、启动代码及 ABI 头文件 |
| `userland/` | 客机用户态、Linux 差分、libc／应用程序的构建与执行；`inventory/` 管理原程序清单 |
| `workloads/` | 按子系统组织的用户态消费者；不保存每轮实验的版本副本 |
| `diff-abi/` | 固定 Linux 参考、同 ELF 对照及 syscall 边界用例 |
| `smp/` | 多核启动、真实用户执行和受控并发组合的执行器 |
| `cost/` | 可复用性能程序、测量和解析工具；程序在 `cost/workloads/`，记账消费者在 `cost/accounting/` |
| `ci/` | CI 分组、依赖准备及运行器检查 |
| `tools/` | 栈／代码布局检查、输入校验和构建产物清理 |

根目录的七个 Python 模块供不同执行器共用：架构配置、宿主绑核、测量互斥、随机种子、运行环境、网络输入与 NBD 控制。

## 使用

```sh
make test-mm-pin-host
make test-page-cache-concurrency-host
make test-user-smp-riscv
python3 -B tests/userland/run.py --help
python3 -B tests/cost/cost-riscv.py --help
```

先选当前问题对应的入口。新增用例优先加入现有消费者；单轮编排、临时探针和报告生成放在 `build/`。运行盘、日志、ELF、缓存也放在 `build/`，不放入本目录。

内核机制见 `docs/modules/`，性能记录由兼容分支的既有报告维护。历史实验按对应 Git 提交复现。
