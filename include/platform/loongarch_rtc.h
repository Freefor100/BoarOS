#ifndef BOAROS_PLATFORM_LOONGARCH_RTC_H
#define BOAROS_PLATFORM_LOONGARCH_RTC_H
#include <stdint.h>
/* No allocation or IRQ owner: boot enables TOY; reads preserve output on failure. */
int la_virt_rtc_initialize(uint64_t *);
int arch_rtc_read_ns(uint64_t *);
/* Hardware access boundary also used by the independent register model. */
uint32_t la_rtc_read32(unsigned offset);
void la_rtc_write32(unsigned offset,uint32_t value);
#endif
