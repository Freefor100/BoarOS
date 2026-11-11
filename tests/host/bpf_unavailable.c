/* Ordinary network ownership fixtures never attach a BPF program. Keep their
 * production NULL-filter path, and fail if a new case actually needs BPF. */
#include <stdio.h>
#include <stdlib.h>
#define EXCLUDED(name) void __wrap_##name(void) {fputs("unopened BPF consumer: " #name "\n",stderr);abort();}
EXCLUDED(kernel_bpf_program_get)
EXCLUDED(kernel_bpf_program_put)
EXCLUDED(kernel_bpf_filter)
