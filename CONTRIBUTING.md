# 参与开发与贡献指南

欢迎参与 BoarOS 的开发与维护。本文说明仓库的编码约定、验证流程、文档规范与 Git 提交格式。系统总体设计见 [系统总体架构与核心设计](docs/design.md)，性能分析方法见 [性能工程与关键优化](docs/performance.md)，自动化工作流约定见 [AGENTS.md](AGENTS.md)。

---

## 一、代码风格与设计习惯

- **语言与汇编边界**：使用 GNU C11 编写内核主体；体系结构汇编仅保留在启动引导、异常陷入、上下文切换、TLB 重填与用户拷贝等必要边界，保持精简。
- **清晰命名**：函数、文件与数据结构命名直接表达职责，避免使用 `manager`、`service`、`helper`、`util` 等含糊后缀。
- **有价值的注释**：注释重点说明设计意图、不变量、所有权归属、并发锁序与硬件约束，不逐行翻译显而易见的 C 代码。
- **严谨边界与回滚**：完整保留 Linux ABI 要求的错误码顺序、参数校验、失败原子回滚、引用计数归还与并发语义。
- **克制抽象**：仅在多个真实调用点呈现相同且稳定的语义时抽取公共接口；体系结构差异通过 `include/arch/` 的构建期契约隔离，不提前设计无实际调用者的过度抽象层。

---

## 二、测试与验证约定

BoarOS 的四层验证体系详见 [正确性保证与四层验证体系](docs/verification.md)，各测试目录职责见 [测试目录说明](tests/README.md)。

- **按风险选择回归范围**：从与本次改动直接相关的宿主/客机检查开始，再按影响面扩大验证；绝不为通过特定测例编写按测试名或固定输入分支的特判代码。
- **反例优先**：并发验证通过确定性握手精确构造交错，校验实际状态登记、完成语义与资源归还，不以随机 `sleep` 或单次打印推断并发正确。
- **测试目录保洁**：`tests/` 只维护长期可复用的行为契约、夹具与执行器；一次性探针、单轮组合日志与临时运行盘统一放在被 Git 忽略的 `build/` 目录下。
- **分工明确**：功能测试验证行为正确性，源码分析评估算法与锁粒度，受控对照实验（关闭诊断后复跑）验证实际吞吐、延迟与资源代价。

常用验证与清理命令：

```sh
make test-riscv      # RISC-V 64 核心回归套件
make test-loongarch  # LoongArch 64 核心回归套件
make prune-build     # 清理可重建的构建产物与临时实验目录（保留交互根盘与工具缓存）
```

---

## 三、文档与分支规范

- **分支职责**：所有通用内核与通用文档改动合入 `main`，再单向合入 `oscomp-compat`；比赛评测适配、判分脚本与测例分析归属 `oscomp-compat` 分支。
- **文档定位**：
  - 顶层文档（`README.md`、`docs/design.md`、`docs/performance.md`、`docs/verification.md`）清晰阐述整体架构、优化逻辑与验证闭环。
  - 子系统文档（`docs/modules/`）按“定位 → 核心对象与流程 → 并发与优化 → 接口契约与验证入口”组织，用通用操作系统概念解释 Linux 专属接口，避免写成按时间堆砌的流水账。
  - 外部第三方代码引入前须同步更新 [第三方组件与许可](docs/third-party.md)，参考源码清单统一维护在 `references/sources.tsv`。

---

## 四、Git 提交规范

- **原子提交**：每次提交聚焦解决一个可清晰说明、可独立验证的问题；纯格式化改动不与功能或性能修改混在同一提交中。
- **标题格式**：采用英文 `type(scope): subject` 形式（`scope` 按需填写），简明概括结果。
- **正文说明**：除拼写或格式微调外，标题空一行后须包含说明正文，简述改动动机、关键机制与约束，并以 `Validation:` 段记录本次验证范围及仍存在的限制。合并提交保留 `Merge ...` 标题并说明集成边界。
- **Trailer 格式**：标题、正文与连续 trailer 之间各保留一个空行，trailer 之间不插空行，文件末尾保留换行；`Co-authored-by` 等 trailer 不替代说明正文。

AI 协作 Trailer 示例：

```text
Co-authored-by: GPT-5.6 Sol <codex@openai.com>
Co-authored-by: GPT-5.6 Luna <codex@openai.com>
Co-authored-by: GPT-6 Astra <codex@openai.com>
Co-authored-by: GPT-6.1 Sol <codex@openai.com>
Co-authored-by: GPT-6 Sol <codex@openai.com>
Co-authored-by: GPT-6 Luna <codex@openai.com>
Co-authored-by: GLM 5.3 <noreply@z.ai>
Co-authored-by: DeepSeek V4 Flash <noreply@deepseek.com>
Assisted-by: Google Antigravity
```
