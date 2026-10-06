#ifndef BOAROS_ARCH_LOONGARCH_FPU_H
#define BOAROS_ARCH_LOONGARCH_FPU_H
#define LA_FPU_FCC_OFFSET 1024
#define LA_FPU_FCSR_OFFSET 1032
#define LA_FPU_WIDTH_OFFSET 1036
#define LA_FPU_SAVED_OFFSET 1040
#define LA_FPU_STATE_SIZE 1056
#ifndef __ASSEMBLER__
#include <arch/task.h>
void la_fpu_save(struct arch_fpu_state *state);
void la_fpu_restore(const struct arch_fpu_state *state);
int la_fpu_first_use(unsigned width);
uint64_t la_user_hwcap(void);
uint32_t la_fpu_take_exception(void);
#endif
#endif
