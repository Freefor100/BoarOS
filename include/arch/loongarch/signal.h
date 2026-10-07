#ifndef BOAROS_ARCH_LOONGARCH_SIGNAL_H
#define BOAROS_ARCH_LOONGARCH_SIGNAL_H
#include <arch/task.h>
void la_signal_prepare_user_return(struct arch_trap_frame *frame);
void la_signal_restore_current(struct arch_trap_frame *frame);
void la_signal_note_address_error(uint32_t access);
#endif
