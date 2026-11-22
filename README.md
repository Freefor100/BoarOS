# BoarOS OSComp 评测兼容分支

`oscomp-compat` 分支专门维护全国大学生计算机系统能力大赛操作系统内核赛（OSComp）的双架构交付构建、双盘启动引导、测例监督框架与判分环境适配。所有通用操作系统内核代码均在 [main 分支](https://github.com/Freefor100/BoarOS/tree/main) 开发验证后再单向合入本分支，确保通用内核的架构纯粹性与比赛交付的可复现性。

---

## 官方容器离线构建

本分支的默认交付入口为 `make all`。在仓库根目录执行以下命令，即可在 [官方评测构建镜像](https://github.com/oscomp/autotest-for-oskernel) 中以完全离线方式生成全部双架构提交产物：

```sh
docker run --rm --network none --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$PWD,target=/coursegrader/submit" \
  --workdir /coursegrader/submit \
  zhouzhouyi/os-contest:20260510 make all
```

构建完成后，仓库根目录将生成四份标准交付文件：

| 产物文件 | 适用架构 | 说明 |
| :--- | :--- | :--- |
| `kernel-rv` | RISC-V 64 | 内嵌评测启动配置的 RISC-V 64 位内核镜像 |
| `kernel-la` | LoongArch 64 | 内嵌评测启动配置的 LoongArch 64 位内核镜像 |
| `disk.img` | RISC-V 64 | 2,304 MiB MBR 辅助盘（工具分区 + FAT32 分区 + 2 GiB LTP 格式化分区 + 一次性启动熵种子） |
| `disk-la.img` | LoongArch 64 | 对应 LoongArch 64 架构的 2,304 MiB MBR 辅助盘 |

在官方评测流程中，两张辅助盘由评测框架作为第二块虚拟磁盘挂载到虚拟机内；原始测试镜像（根盘）保持原样不被修改。

---

## 本地评测与常用入口

| 命令 | 用途 |
| :--- | :--- |
| `make all` | 离线构建 `kernel-rv`、`kernel-la`、`disk.img` 与 `disk-la.img` |
| `make test-oscomp-host` | 运行评测脚本、磁盘布局、工具哈希与引导逻辑的宿主单元测试 |
| `make test-oscomp-compat` | 使用本地 QEMU 串行运行 RISC-V 与 LoongArch 初赛全组诊断 |
| `make test-oscomp-pre` | 在官方容器环境中以只读原盘 + QCOW2 写时复制层复现初赛官方流程与判分 |
| `make test-oscomp-final` | 启动 8 核 / 8 GiB 内存配置运行 `final-2026` 公开镜像（CAgent 与 BuildStorm） |

---

## 专题文档索引

- **双盘布局、熵注入、监督器与初/决赛运行入口**：[OSComp 评测环境适配](docs/modules/oscomp-compat.md)
- **发布镜像、C 运行库、系统调用 ABI 与判分问题归因**：[OSComp 测例与环境问题](docs/learning/oscomp-test-issues.md)
- **通用内核设计、开发环境与交互式启动**：参见 [main 分支文档](https://github.com/Freefor100/BoarOS/tree/main)
