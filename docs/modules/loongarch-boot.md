# LoongArch QEMU 启动

当前入口为`arch/loongarch/boot.S`与`main.c`；平台事实独立放在
`platform/loongarch_virt.c`。`make kernel-la`构建，`make test-loongarch-boot`
运行512MiB和1GiB的独立启动、真实RAM写读、分配和释放基线检查。

内核物理装载地址为2MiB，高地址为缓存DMW的`0x9000000000200000`。
开启分页前临时建立低地址DMW，跳到高地址后立即撤销；用户模式不可使用
内核PLV0的DMW。平台从直接启动传入的EFI system table定位DTB，检查签名、
表数与范围；按DTB中所有RAM bank排序，扣除启动信息、DTB、内核和保留区。
物理分配器复用通用buddy实现，页大小在构建期固定为16KiB。

当前只验收启动和物理页回收，尚未验收三级用户页表、timer、调度或用户ELF。
`test-loongarch`是后续完整首阶段入口；现阶段预期缺少用户态结果。

固定依据为`references/qemu` v11.1.0，commit
`84f07211cc5b4fc6a371559bf8a5de4fb068e648`的`hw/loongarch/boot.c`、
`include/hw/loongarch/virt.h`和FDT生成代码。工具为本机
`loongarch64-unknown-linux-gnu-gcc`15.1.0；内核使用LP64S、禁用浮点和LSX/LASX。
QEMU从该固定源码在`build/qemu-la`构建，未引入新的产品依赖。
