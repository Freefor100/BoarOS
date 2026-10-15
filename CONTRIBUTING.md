# 参与开发


## 修改流程

工作流程见 [Agent 约束](AGENTS.md)，技术原则见[设计与工程原则](docs/design.md)。先判断问题和目标结构，再决定修改范围；已授权目标内的普通实现选择和必要内部调整直接完成，需要用户决定的实质取舍再讨论。

## 代码习惯

- 使用 GNU C11 和必要的架构汇编；汇编保持最小。
- 名字说明具体职责，避免 `manager`、`service`、`helper`、`util` 等模糊容器。
- 注释解释意图、不变量、所有权、并发和硬件约束，不逐行翻译代码。
- 保留准确的错误码、边界检查、失败回滚、资源释放和并发语义。
- 只有真实使用点出现相同且稳定的语义时才抽取公共接口。
- 架构差异与通用机制分离，但不预建尚未需要的抽象层。

## 测试

- `tests/` 中由项目维护的测试源码、fixture 和 runner 是可执行契约，必须纳入 Git，并与被验证行为一起维护。
- 重构后继续检查原有行为和必要约束；依赖私有布局、旧接口或偶然调用顺序的用例允许随实现改写，失去独立检错价值的用例应删除。行为要求改变时同步更新实现与测试；禁止按测试名称或固定输入写特判，也不为通过检查而放宽应有行为。
- 构建产物、临时日志、磁盘镜像和测试下载物不纳入 Git；外部比赛测例保存在被忽略的 `references/`，通过固定来源恢复。
- 架构或平台专用 runner 用来补充真实启动边界，不能替代通用模块的语义测试。
- 并发反例通过握手强制交错，检查实际登记/提交、完成结果和owner归还，不以打印ready或随机sleep代替。历史异常的归因未确认时保留证据边界，后续通过不能改写原失败。
- 测试检查行为，源码与设计分析判断算法和结构，运行测量回答实际性能与资源代价；按当前问题选择，不以完整成本检查作为修改的前置条件。具体原则见[设计与工程原则](docs/design.md#成本与证据)。

测试从直接相关的检查开始，再根据风险扩大。LTP 和比赛测例是语义探针；失败应先还原成与测试名称无关的问题。物理页分配的聚焦入口是 `make test-page-riscv`，完整 RISC-V 启动入口是 `make test-riscv`。

## 文档与提交

文档职责和触发条件见[文档导航](docs/README.md)。不要复制源码清单、创建空模板或把临时思考固化成永久规则。

一个提交解决一个可说明、可验证的问题；纯格式化不与机制修改混合。外部代码引入前更新[第三方代码](docs/third-party.md)。

提交标题概括结果。除拼写、格式等从标题和 diff 即可完整理解的小修外，提交必须在空行后写说明性 body，简要交代改动动机、关键行为或约束，以及与风险相称的验证和仍存限制；`Co-authored-by` 等 trailer 不算正文。

提交沿用英文 `type(scope): subject` 标题，`scope` 按需要填写；正文说明原因、行为和约束，以 `Validation:` 段记录验证及限制。合并提交保留 `Merge ...` 标题并说明集成边界。

标题、正文、连续 trailer 之间各留一个空行，trailer 之间不插入空行，文件以换行结束；trailer 不代替说明正文。

AI 对设计或代码有实质贡献时填写具体Agent型号，不使用笼统的`GPT-6`。例如：

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
