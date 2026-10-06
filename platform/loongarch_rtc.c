#include <platform/loongarch_rtc.h>
#include <platform/loongarch_virt.h>
#include <stddef.h>
#define RTC_BASE (LA_UNCACHED_BASE+UINT64_C(0x100d0100))
#define TOY_ENABLE UINT32_C(0x900)
__attribute__((weak)) uint32_t la_rtc_read32(unsigned offset)
{ return *(volatile uint32_t *)(uintptr_t)(RTC_BASE+offset); }
__attribute__((weak)) void la_rtc_write32(unsigned offset,uint32_t value)
{ *(volatile uint32_t *)(uintptr_t)(RTC_BASE+offset)=value; }
static unsigned leap(unsigned year)
{ return !(year%4) && (year%100 || !(year%400)); }
static int utc(uint32_t high,uint32_t low,uint64_t *out)
{
    /* TOY年字段为tm_year；限制范围同时防止无界循环与纳秒溢出。 */
    if(high<70 || high>654)return 1;
    unsigned year=high+1900,month=low>>26,day=(low>>21)&31;
    unsigned hour=(low>>16)&31,minute=(low>>10)&63,second=(low>>4)&63;
    static const unsigned lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(!month || month>12 || !day || day>lengths[month-1]+(month==2 && leap(year)) ||
        hour>23 || minute>59 || second>59)return 1;
    uint64_t days=0;
    for(unsigned y=1970;y<year;y++)days+=365+leap(y);
    for(unsigned m=1;m<month;m++)days+=lengths[m-1]+(m==2 && leap(year));
    days+=day-1;
    uint64_t seconds=days*86400+hour*3600+minute*60+second;
    if(seconds>UINT64_MAX/UINT64_C(1000000000))return 1;
    *out=seconds*UINT64_C(1000000000);return 0;
}
int arch_rtc_read_ns(uint64_t *out)
{
    if(!out)return 1;
    /* READ0/1没有锁存。完整样本重复一致才接受，避免跨秒/年拼接日期。 */
    for(unsigned attempt=0;attempt<8;attempt++) {
        if((la_rtc_read32(0x40)&TOY_ENABLE)!=TOY_ENABLE)return 1;
        uint32_t high=la_rtc_read32(0x30),low=la_rtc_read32(0x2c);
        uint32_t next_high=la_rtc_read32(0x30),next_low=la_rtc_read32(0x2c);
        if((la_rtc_read32(0x40)&TOY_ENABLE)!=TOY_ENABLE)return 1;
        if(high==next_high && low==next_low)return utc(high,low,out);
    }
    return 1;
}
int la_virt_rtc_initialize(uint64_t *out)
{
    if(!out)return 1;
    /* 仅启动owner启用TOY/晶振；查询失败不能伪造日期或重启已关闭设备。 */
    la_rtc_write32(0x20,0);
    la_rtc_write32(0x40,la_rtc_read32(0x40)|TOY_ENABLE);
    return arch_rtc_read_ns(out);
}
