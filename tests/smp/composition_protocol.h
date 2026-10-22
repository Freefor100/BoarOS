#ifndef BOAROS_COMPOSITION_PROTOCOL_H
#define BOAROS_COMPOSITION_PROTOCOL_H
#include <stdint.h>
#define COMPOSITION_MAGIC UINT64_C(0x424f4152434f4d50)
enum composition_command { COMPOSITION_PRESSURE=1, COMPOSITION_RELEASE,
    COMPOSITION_OOM, COMPOSITION_RFENCE, COMPOSITION_FAULT_RACE,
    COMPOSITION_FILE_FAULT_RACE, COMPOSITION_READ_ARM, COMPOSITION_READ_FINISH,
    COMPOSITION_WAIT_ARM, COMPOSITION_WAIT_FINISH, COMPOSITION_CLOCK_ARM, COMPOSITION_CLOCK_FINISH, COMPOSITION_STOP, COMPOSITION_SOCKET_WAIT };
enum composition_read_flags { COMPOSITION_READ_LOCAL=1, COMPOSITION_WRITE_LOCAL=2,
    COMPOSITION_READ_HOLD=4, COMPOSITION_READ_CLOSE=8, COMPOSITION_READ_FORK=16,
    COMPOSITION_READ_SEEK=32, COMPOSITION_READ_COLD=64, COMPOSITION_READ_PREFIX=128, COMPOSITION_READ_BATCH=256,
    COMPOSITION_READ_SIGNAL=512 };
/* 私有tmpfs控制页：映射/OFD与控制器额外页引用分别覆盖各自使用寿命。
 * request/reply是单控制者release/acquire交接，不作为生产ABI。 */
struct composition_control {
    uint64_t magic, observer_ready, request, reply, command, target_tid, target_address;
    uint64_t retirement_entered, retirement_release, remote_progress;
    uint64_t epoch, registered, participants[32];
    uint64_t fault_address, fault_tasks[2], fault_file_paused;
    uint64_t read_tid, read_address, read_size, read_hart, read_flags;
    uint64_t read_entered, read_release, read_waiter_tid, read_waited;
    uint64_t wait_tid, wait_child, wait_address, wait_entered, wait_release;
    uint64_t clock_tid, clock_pid, clock_entered, clock_release;
    uint64_t socket_tid, socket_mode, socket_entered, socket_release, socket_waiter_tid, socket_waited;
};
static inline uint64_t composition_load(const uint64_t *p)
{ return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static inline void composition_store(uint64_t *p,uint64_t v)
{ __atomic_store_n(p,v,__ATOMIC_RELEASE); }
#endif
