#include "abi.h"

struct coarse_time { long sec, ns; };
static unsigned long nanos(struct coarse_time t)
{ return (unsigned long)t.sec * 1000000000UL + (unsigned long)t.ns; }

void abi_coarse_cases(void)
{
    static const char *const get_names[] = {"coarse.realtime.get", "coarse.monotonic.get"};
    static const char *const res_names[] = {"coarse.realtime.res", "coarse.monotonic.res"};
    static const char *const null_names[] = {"coarse.realtime.res-null", "coarse.monotonic.res-null"};
    static const char *const fault_names[] = {"coarse.realtime.bad-pointer", "coarse.monotonic.bad-pointer"};
    static const char *const sleep_names[] = {"coarse.realtime.sleep", "coarse.monotonic.sleep"};
    static const char *const abs_names[] = {"coarse.realtime.sleep-absolute", "coarse.monotonic.sleep-absolute"};
    static const char *const bad_sleep_names[] = {"coarse.realtime.sleep-bad-pointer", "coarse.monotonic.sleep-bad-pointer"};
    for (unsigned i=0; i<2; i++) {
        long id=5+i;
        struct coarse_time first, last, fine, resolution;
        long got=SC2(113,id,&first);
        long properties[4]={0,0,0,0};
        if (!got) {
            abi_require(SC2(113,i,&fine)==0);
            properties[0]=first.sec>=0 && first.ns>=0 && first.ns<1000000000;
            properties[1]=nanos(first)<=nanos(fine);
            properties[2]=1;
            for (unsigned sample=0; sample<32; sample++) {
                abi_require(SC2(113,id,&last)==0);
                properties[2] &= nanos(last)>=nanos(first);
                first=last;
            }
            struct coarse_time wait={0,50000000};
            abi_require(SC2(101,&wait,0)==0 && SC2(113,id,&last)==0);
            properties[3]=nanos(last)>nanos(first);
        }
        abi_record(get_names[i],got,-1,-1,0,properties,sizeof(properties));
        got=SC2(114,id,&resolution);
        /* Linux HZ=250 and BoarOS HZ=100 have different honest resolutions. */
        long honest=!got && !resolution.sec && resolution.ns>1 && resolution.ns<=10000000;
        if (!got) abi_require(SC2(114,id,1)==-14);
        abi_record(res_names[i],got,-1,-1,0,&honest,sizeof(honest));
        abi_record(null_names[i],SC2(114,id,0),-1,-1,0,0,0);
        abi_record(fault_names[i],SC2(113,id,1),-1,-1,0,0,0);
        struct coarse_time zero={0,0};
        abi_record(sleep_names[i],SC4(115,id,0,&zero,0),-1,-1,0,0,0);
        abi_record(abs_names[i],SC4(115,id,1,&zero,0),-1,-1,0,0,0);
        abi_record(bad_sleep_names[i],SC4(115,id,0,1,0),-1,-1,0,0,0);
    }
}
