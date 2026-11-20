# BoarOS 的 OSComp 评测分支

`oscomp-compat` 专门维护 OSComp 操作系统内核赛的交付构建、启动配置、测例监督和判分适配。通用内核从 `main` 单向合入。项目介绍、开发环境和交互启动见 [main 分支](https://github.com/Freefor100/BoarOS/tree/main)。

## 官方镜像构建

比赛的构建入口是 `make all`。在本分支的仓库根目录使用下面的命令，可在官方初赛镜像中生成交付产物。 [官方构建环境](https://github.com/oscomp/autotest-for-oskernel)

```sh
docker run --rm --network none --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$PWD,target=/coursegrader/submit" \
  --workdir /coursegrader/submit \
  zhouzhouyi/os-contest:20260510 make all
```

构建生成 `kernel-rv`、`kernel-la`、`disk.img` 和 `disk-la.img`。两份内核各自使用评测启动配置，两张辅助盘由框架作为第二块磁盘挂载。初赛与决赛的原盘、启动入口和适配范围见[评测环境适配](docs/modules/oscomp-compat.md)。发布盘、运行库和判分问题见[已知测例问题](docs/learning/oscomp-test-issues.md)。
