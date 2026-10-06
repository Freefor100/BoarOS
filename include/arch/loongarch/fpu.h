#ifndef BOAROS_ARCH_LOONGARCH_FPU_H
#define BOAROS_ARCH_LOONGARCH_FPU_H
#define LA_FPU_FCC_OFFSET 256
#define LA_FPU_FCSR_OFFSET 264
#define LA_FPU_SAVED_OFFSET 272
#define LA_FPU_STATE_SIZE 288
#ifndef __ASSEMBLER__
#include <arch/task.h>
void la_fpu_save(struct arch_fpu_state *state);
void la_fpu_restore(const struct arch_fpu_state *state);
void la_fpu_first_use(void);
uint32_t la_fpu_take_exception(void);
#endif
#endif
