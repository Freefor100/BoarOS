#ifndef BOAROS_COMPOSITION_PROTOCOL_H
#define BOAROS_COMPOSITION_PROTOCOL_H
#include <stdint.h>
#define COMPOSITION_MAGIC UINT64_C(0x424f4152434f4d50)
enum composition_command { COMPOSITION_PRESSURE=1, COMPOSITION_RELEASE,
    COMPOSITION_OOM, COMPOSITION_RFENCE, COMPOSITION_FAULT_RACE,
    COMPOSITION_FILE_FAULT_RACE, COMPOSITION_STOP };
/* 私有tmpfs控制页：映射/OFD与控制器额外页引用分别覆盖各自使用寿命。
 * request/reply是单控制者release/acquire交接，不作为生产ABI。 */
struct composition_control {
    uint64_t magic, observer_ready, request, reply, command, target_tid, target_address;
    uint64_t retirement_entered, retirement_release, remote_progress;
    uint64_t epoch, registered, participants[32];
    uint64_t fault_address, fault_tasks[2], fault_file_paused;
};
static inline uint64_t composition_load(const uint64_t *p)
{ return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static inline void composition_store(uint64_t *p,uint64_t v)
{ __atomic_store_n(p,v,__ATOMIC_RELEASE); }
#endif
