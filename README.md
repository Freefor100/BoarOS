# BoarOS

<img src="assets/logo-concept.png" alt="BoarOS 正面野猪 Logo" width="180">

BoarOS 是一个从零学习并面向 OS Comp 能力建设的 C + 汇编类 Linux 内核，也是一次长期的人—Agent 协作开发实践。

## 当前状态

项目目标、工程规则和已验证的本地工具链事实已经建立；目前还没有可启动的内核。

## 技术方向

- RISC-V64 + OpenSBI 优先；LoongArch64 随后接入。
- 内核使用 GNU C11 和必要的架构汇编。
- 先形成可观察、可验证的端到端能力，再依据真实需求扩展。
- 测例用于发现通用语义问题，不作为硬编码实现清单。

## 文档

- [目标与边界](docs/goals.md)
- [设计与工程原则](docs/design.md)
- [工具链事实](docs/toolchain.md)
- [文档导航](docs/README.md)
- [参与开发](CONTRIBUTING.md)

`references/` 保存本地规则、Harness 和公开测例快照，不纳入版本控制。

## 公开参考

- [OS Comp 2026 规则](https://gitlab.eduxiji.net/csc1/csc-os/os2026)
- [OS Comp 内核公开测例](https://github.com/oscomp/testsuits-for-oskernel)
- [OS Comp 自动测试 Harness](https://github.com/oscomp/autotest-for-oskernel)
