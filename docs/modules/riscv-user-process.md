# RISC-V 用户进程资源模块

本文描述当前 RISC-V 最小进程资源容器的稳定接口和所有权状态。它只解决“用户地址空间由谁持有和回收”，不把 PID、父子关系、文件表等尚未实现的 Linux 进程语义伪装成现有能力。用户页表本身的契约见 [RISC-V Sv39 分页模块](riscv-sv39.md)，可调度线程见 [内核线程调度模块](kernel-scheduler.md)。

## 范围与接口

| 文件 | 当前职责 |
|---|---|
| `include/arch/riscv/user_process.h`、`arch/riscv/user_process.c` | 用可移动句柄管理独立进程记录页及其中的 Sv39 用户空间 |
| `kernel/scheduler.c` | 用户线程创建成功后持有进程句柄，退出后在内核根和 idle 栈上销毁它 |
| `tests/riscv/user_process_cases.c`、`tests/user-process-riscv.sh` | 验证创建、查询、移动、销毁和复合失败的所有权闭环 |

```c
enum riscv_user_process_status riscv_user_process_create(
    struct riscv_user_process *process,
    struct riscv_sv39_user_space *space);

enum riscv_user_process_status riscv_user_process_move(
    struct riscv_user_process *destination,
    struct riscv_user_process *source);

enum riscv_user_process_status riscv_user_process_satp(
    const struct riscv_user_process *process,
    uint64_t *satp);

enum riscv_user_process_status riscv_user_process_lookup(
    const struct riscv_user_process *process,
    uint64_t virtual_address,
    struct riscv_sv39_mapping *mapping);

enum riscv_user_process_status riscv_user_process_destroy(
    struct riscv_user_process *process);
```

公开句柄只保存分配器、进程记录页物理地址和状态，不暴露记录页的长期虚拟指针。记录页固定占用一个 4 KiB 物理页，内部以 magic 校验并持有一个 `riscv_sv39_user_space`。当前 scheduler 把任务 `satp` 缓存在固定线程前缀中，切换本身不需要从进程记录推导根页号；调度前的不变量检查仍会解析记录并核对缓存，退出回收也通过句柄访问记录。

## 状态与所有权

```text
EMPTY --create成功，move输入空间--> LIVE --move--> MOVED
                                      |
                                      +--destroy全部成功--> DESTROYED
                                      +--空间已销毁但记录页释放失败--> CLEANUP

EMPTY --记录页已分配，访问和立即释放同时失败--> CLEANUP
CLEANUP --move--> MOVED
        +--destroy重试成功--> DESTROYED
```

`create` 只接受精确清零的 EMPTY 句柄和 LIVE 用户空间。成功时输入空间变为 MOVED，记录页成为唯一所有者；普通失败保持两者不变。若记录页已经分配，但访问和立即释放同时失败，输出进程进入 CLEANUP 并只拥有这张原始记录页，输入用户空间仍由调用者持有。调用者必须分别重试销毁 CLEANUP 句柄和原用户空间。

LIVE 句柄的 `satp` 和 `lookup` 每次解析记录页并验证 magic、分配器和内部空间状态；未映射地址与记录页不可访问使用不同状态码，失败不修改输出。`move` 只在完整成功时转移 LIVE 或 CLEANUP 所有权，源变为 MOVED。

`destroy` 先销毁非活动用户空间，再释放记录页。如果记录页暂时不可访问，或用户页表回收中途失败，句柄保持 LIVE 并可重试。若地址空间已经完全销毁而记录页释放失败，句柄进入 CLEANUP；此后只重试释放记录页，不会二次销毁空间。成功后句柄清除分配器和物理地址并进入 DESTROYED。物理地址 0 可以是合法记录页，所有权判断依赖状态而不是地址非零。

## scheduler 集成与限制

完整所有权链为：

```text
ELF loader -> LIVE user space
process_create -> LIVE process + MOVED user space
kernel_user_thread_create -> thread owns process + MOVED caller handle
idle reaper -> destroy process -> release thread page
```

线程创建在移动进程前完成入口 U+X、栈 U+RW、`satp`、分配器、线程页和初始 Frame 校验；失败时进程仍由调用者持有。用户任务退出后先切回其他地址空间和可信栈，reaper 才销毁进程。进程销毁失败时 exited 节点留在队首；进程已经 DESTROYED 而线程页释放失败时，重试不会二次销毁进程。

```sh
make test-user-process-riscv
make test-user-riscv
make test-user-elf-riscv
make test-riscv
```

聚焦测试会注入记录页首次访问失败、访问与立即释放同时失败、销毁期间记录页不可访问、内部页表不可访问，以及地址空间销毁成功后记录页释放失败；每种情况都检查句柄状态、输入空间状态和物理空闲页数。真实 U-mode 测试再在 scheduler exited 队列中分别注入地址空间部分回收、记录页释放和线程页释放失败，验证节点保留、准确错误码、完成输出不变及重试闭环；ELF 测试验证成功路径能跨 `satp` 运行并回收全部页。

当前固定一个进程对应一个用户线程，没有 PID、父子关系、引用计数、共享地址空间多线程、文件表、当前工作目录、凭据、信号、`fork/exec/wait` 生命周期或 LoongArch 进程记录。加入第二种架构实现前不建立通用进程 vtable；RISC-V 句柄也不预留这些尚不存在的字段。
