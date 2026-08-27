# DTB 与启动内存布局模块

本文描述启动期 DTB 快照的稳定接口：读取器保存内存、timebase、保留区和 VirtIO MMIO transport 事实，启动布局构建器再排除固件、内核镜像和 DTB 自身占用。相关内存原理见[内存管理学习总结](../learning/memory-management.md)，设备消费者见[RISC-V VirtIO MMIO 块设备模块](riscv-virtio-block.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/dtb.h`、`kernel/dtb.c` | 校验 DTB，读取第一段 RAM、DTB 大小、RISC-V timebase、静态保留区和 VirtIO MMIO 范围 |
| `include/kernel/boot_memory.h`、`kernel/boot_memory.c` | 规范化保留区并生成字节粒度的可用区间 |
| `kernel/main.c` | 加入内核与 DTB 占用，输出启动布局诊断 |
| `tests/riscv/dtb_main.c` | 用合成 DTB 验证格式、状态和输出契约 |
| `tests/riscv/boot_memory_cases.c` | 验证区间裁剪、排序、合并与相减 |
| `tests/dtb-riscv.sh`、`tests/boot-riscv.sh` | 运行聚焦测试和真实 QEMU 启动测试 |

`dtb_read_boot_info` 成功时写出第一段非空 RAM、header 的 `total_size`、根节点直属 `/cpus/timebase-frequency`，以及 memory reservation block 和静态 `/reserved-memory/*/reg` 中的非空区间。两种保留区来源合计最多 16 项；超限返回 `DTB_STATUS_UNSUPPORTED`。任何失败都不修改输出。

`timebase-frequency` 缺失时字段为零，通用 DTB 读取仍成功；RISC-V timer 启动负责把零频率作为平台缺失错误。属性存在时必须恰为一个非零 32 位 cell，错误长度或重复属性返回 `DTB_STATUS_INVALID`。其他节点中的同名属性不冒充 `/cpus` 值。

设备 walker 接受 NUL 分隔 compatible string list，选择包含 `virtio,mmio` 且 `status` 缺失或为 `okay`/`ok` 的节点。它继承父节点 address/size cells，按每层非空 `ranges` 把子总线地址翻译到根物理地址；最多 16 层、16 个 transport。结果按物理地址排序，空范围、溢出、无法翻译或重叠都拒绝，失败仍不修改输出。

`boot_memory_build` 接收上述快照、链接器给出的半开内核区间和 DTB 地址。成功时返回按地址排序的 `reserved[]` 与 `usable[]`；失败区分非法输入和没有剩余物理内存，输出同样保持不变。

## 格式与布局不变量

- 读取器按大端格式逐字段解码，不把可能未对齐的 DTB 数据转换成 C 结构。
- header 总长度、块顺序与范围、对齐、版本、reservation terminator、结构 token、字符串终止和属性填充都受边界检查。
- 根节点缺省使用两个 address cell 和一个 size cell；显式值只能是一或二。memory 节点必须位于根节点下，并提供合法的 `device_type = "memory"` 与 `reg`。
- `timebase-frequency` 是平台提供的原始计数频率；本模块不计算 tick period，也不假设 QEMU 或开发板频率。
- `/reserved-memory` 必须使用与根节点相同的 cell 数并带空 `ranges`；静态子节点的每个 `reg` tuple 都会保存。动态 `size` 形式和相关节点的 `status` 语义尚未实现，读取器明确返回 unsupported。
- 布局构建器检查所有 `base + size` 运算，要求内核区间完整位于所选 RAM 中；DTB 或设备树保留区位于 RAM 外的部分会被裁掉。
- 保留区排序后合并重叠或相邻项，再从 RAM 中相减。结果保持字节粒度，页边界对齐由后续物理页分配器负责。
- 当前只选择第一段非空 RAM；多 RAM bank 尚未进入当前目标，不会静默拼接或伪装成已支持。
- VirtIO 发现只记录 transport 的物理 `reg`，不读取 MMIO device ID、协商 feature 或决定块设备；这些副作用属于设备驱动。

## 设计依据

- [Devicetree Specification：扁平格式](https://devicetree-specification.readthedocs.io/en/stable/flattened-format.html)、[memory 与 reserved-memory 节点](https://devicetree-specification.readthedocs.io/en/stable/devicenodes.html)
- [OpenSBI v1.8.1 `fdt_fixup.c`](https://github.com/riscv-software-src/opensbi/blob/v1.8.1/lib/utils/fdt/fdt_fixup.c)：OpenSBI 将固件/PMP 保护范围加入 `/reserved-memory`
- Linux `f4cdf7ca9a1f` 的 [`drivers/of/fdt.c`](https://github.com/torvalds/linux/blob/f4cdf7ca9a1fdcca413157df19753f388a5a224e/drivers/of/fdt.c) 与 [`arch/riscv/mm/init.c`](https://github.com/torvalds/linux/blob/f4cdf7ca9a1fdcca413157df19753f388a5a224e/arch/riscv/mm/init.c)：确认 RAM 发现、静态保留区、内核和 DTB 自保留的启动顺序

本地 Linux 固定提交快照保存在被忽略的 `references/linux/`；项目没有复制上述来源的代码，因此不构成第三方源码引入。

## 验证与限制

```sh
make test-dtb-riscv
make test-riscv
```

聚焦测试覆盖一或两个 cell、多 tuple、两类静态保留区、timebase 正常/缺失/错误位置/错误长度/零值/重复、VirtIO compatible list、status、嵌套 ranges、排序/重叠/容量/深度，以及布局的裁剪、合并、耗尽和溢出。完整启动测试在 QEMU `virt` 的 512 MiB、1 GiB 与 16 GiB 配置下验证真实 OpenSBI DTB、10 MHz timebase、原始 RAM 大小和非空启动布局。

本模块仍不处理多 RAM bank、动态 reserved-memory、NUMA、热插拔或 CMA；页边界收缩和单页分配由[物理页分配模块](physical-pages.md)承担。
