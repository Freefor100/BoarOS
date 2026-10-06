#include <platform/loongarch_rtc.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint32_t ctrl,high,low;
static unsigned reads,rollover,unstable,writes;
static uint32_t date(unsigned mon,unsigned day,unsigned hour,unsigned min,unsigned sec)
{return mon<<26 | day<<21 | hour<<16 | min<<10 | sec<<4;}
uint32_t la_rtc_read32(unsigned offset)
{
    if(offset==0x40)return ctrl;
    reads++;
    if(unstable)return offset==0x30 ? 126 : date(1,1,0,0,(reads/2)&1);
    if(rollover && reads==2){high=127;low=date(1,1,0,0,0);}
    return offset==0x30 ? high : low;
}
void la_rtc_write32(unsigned offset,uint32_t value)
{writes++;if(offset==0x40)ctrl=value;else assert(offset==0x20 && !value);}
static void sample(unsigned year,unsigned mon,unsigned day,uint64_t expected)
{
    high=year-1900;low=date(mon,day,0,0,0);reads=0;uint64_t out=123;
    assert(!arch_rtc_read_ns(&out) && out==expected);
}
static void invalid(unsigned year,unsigned mon,unsigned day,unsigned hour,unsigned min,unsigned sec)
{
    high=year-1900;low=date(mon,day,hour,min,sec);uint64_t out=123;
    assert(arch_rtc_read_ns(&out) && out==123);
}
int main(void)
{
    uint64_t out=123;high=126;low=date(1,1,0,0,0);
    assert(arch_rtc_read_ns(&out) && out==123);
    assert(!la_virt_rtc_initialize(&out) && writes==2 && (ctrl&0x900)==0x900);
    sample(1970,1,1,0);sample(2000,2,29,UINT64_C(951782400000000000));
    sample(2100,3,1,UINT64_C(4107542400000000000));
    sample(2026,1,1,UINT64_C(1767225600000000000));
    invalid(1969,12,31,23,59,59);invalid(2555,1,1,0,0,0);
    invalid(2025,2,29,0,0,0);invalid(2100,2,29,0,0,0);
    invalid(2026,0,1,0,0,0);invalid(2026,13,1,0,0,0);invalid(2026,1,0,0,0,0);
    invalid(2026,4,31,0,0,0);invalid(2026,1,1,24,0,0);
    invalid(2026,1,1,0,60,0);invalid(2026,1,1,0,0,60);
    high=126;low=date(12,31,23,59,59);reads=0;rollover=1;
    assert(!arch_rtc_read_ns(&out) && out==UINT64_C(1798761600000000000));rollover=0;
    unstable=1;out=123;assert(arch_rtc_read_ns(&out) && out==123);unstable=0;
    ctrl=0x800;assert(arch_rtc_read_ns(&out));ctrl=0x100;assert(arch_rtc_read_ns(&out));
    assert(arch_rtc_read_ns(0) && la_virt_rtc_initialize(0));
    puts("LS7A RTC: UTC/leap/century/range/rollover/unstable/disabled/output preservation PASS");
}
