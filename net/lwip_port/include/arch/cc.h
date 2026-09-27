#ifndef BOAROS_LWIP_ARCH_CC_H
#define BOAROS_LWIP_ARCH_CC_H

/* The raw API runs in one S-mode execution context on RV64. */
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_NO_CTYPE_H 1
#define LWIP_NO_LIMITS_H 1
#define LWIP_NO_UNISTD_H 1
#define INT_MAX __INT_MAX__
#define LWIP_PLATFORM_DIAG(message) ((void)0)
#define LWIP_PLATFORM_ASSERT(message) __builtin_trap()
#define LWIP_RAND() boaros_lwip_random()

unsigned int boaros_lwip_random(void);

#endif
