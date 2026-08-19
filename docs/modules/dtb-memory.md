# DTB 物理内存发现模块

本文描述当前 DTB 读取器的稳定接口。它只发现固件描述的物理 RAM，不计算可分配内存，也不承担通用设备枚举。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/dtb.h` | 定义读取状态、物理内存范围和公开函数 |
| `kernel/dtb.c` | 校验扁平设备树并扫描根节点的内存子节点 |
| `kernel/main.c` | 消费第一段物理内存并输出启动诊断 |
| `tests/riscv/dtb_main.c` | 构造有效与畸形 DTB，验证公开状态和输出契约 |
| `tests/dtb-riscv.sh` | 在专用 RISC-V 测试内核中运行聚焦解析测试 |
| `tests/boot-riscv.sh` | 用两种真实 QEMU RAM 配置验证解析结果 |

公开接口 `dtb_read_first_memory_range` 接收固件传入的 DTB 地址，成功时写出第一个大小非零的 `base`、`size`，并返回 `DTB_STATUS_OK`。失败状态区分畸形输入、找不到内存和当前不支持的编码；输出参数只有成功时才有效。

## 格式契约与不变量

- 读取器按大端格式解码 DTB header、结构块 token、属性元数据和 cell；不会把 DTB 数据直接转换成可能未对齐的 C 结构。
- header 的总长度、结构块、字符串块、reservation block 的终止项、块顺序、互不重叠、对齐和版本兼容性在扫描前检查；节点名、属性名、属性值和四字节填充的访问均受对应块边界限制。
- 只接受版本不早于 17、且向版本 17 兼容的扁平设备树。
- 根节点缺省使用两个 address cell 和一个 size cell；显式值只能是一或二，因此地址和大小最多为 64 位。
- 属性必须出现在所属节点的子节点之前；读取器拒绝重复的根 cell 属性和 memory 节点必需属性。
- 只识别根节点下名称为 `memory` 或带有非空 unit-address 的 `memory@...` 节点；其 `device_type` 必须是单个、以空字符结尾的 `memory` 字符串，并且必须提供 `reg`。一个 `reg` 可包含多组范围，但接口只返回遇到的第一段非空范围。
- 扫描会继续到唯一的 `FDT_END`，所以找到内存后仍会检查余下结构块是否合法。

这些约束遵循 Devicetree Specification 的 [扁平格式](https://devicetree-specification.readthedocs.io/en/stable/flattened-format.html)与[内存节点](https://devicetree-specification.readthedocs.io/en/stable/devicenodes.html)定义。

## 当前限制与验证

模块不解析 memory reservation block 的条目或 `/reserved-memory`，也不排除 OpenSBI、内核镜像和 DTB 自身占用的区域；因此返回值是物理 RAM，不是页分配器可以直接使用的范围。多段 RAM 虽可被校验，但当前调用者只取得第一段。

```sh
make test-dtb-riscv
make test-riscv
```

`make test-dtb-riscv` 构建专用内核，用合成 DTB 覆盖缺省与单 cell、多个范围、四种公开状态、失败时输出参数不变、块布局、属性顺序与重复、字符串终止和 memory 必需属性。`make test-riscv` 会先运行该聚焦测试，再在 QEMU `virt` 的 512 MiB 和 1 GiB 配置下启动真实内核，要求解析结果分别为 `0x80000000 + 0x20000000` 和 `0x80000000 + 0x40000000`，同时保留原有 hart ID、DTB 地址变化与 SBI 关机断言。
