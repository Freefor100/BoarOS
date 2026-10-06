#include "abi.h"
struct stamp { long seconds; unsigned nanoseconds; int reserved; };
struct statx_result {
    unsigned mask, blksize; unsigned long attributes;
    unsigned nlink,uid,gid;unsigned short mode,pad;
    unsigned long ino,size,blocks,attributes_mask;
    struct stamp atime,btime,ctime,mtime;
    unsigned rdev_major,rdev_minor,dev_major,dev_minor;
    unsigned long spare[14];
};
_Static_assert(sizeof(struct statx_result)==256,"statx wire ABI");
void abi_statx_cases(void)
{
    long fd=abi_open("/statx-data",0102|01000);abi_require(fd>=0);
    abi_require(SC3(64,fd,"statxdata",9)==9);
    abi_require(SC3(36,"/statx-data",-100,"/statx-link")==0);
    struct statx_result st;
    const long flags[]={0,0x2000,0x4000,0x800};
    const char *ids[]={"statx.path","statx.force","statx.cached","statx.noautomount"};
    for(unsigned i=0;i<4;i++) {
        long r=SC5(291,-100,"/statx-data",flags[i],0x7ff,&st);
        abi_record(ids[i],r,r ? -1 : (long)st.size,r ? -1 : st.mode&0170000,0,0,0);
        if(!r)abi_require((st.mask&0x7ff)==0x7ff && st.uid==0 && st.gid==0 && st.nlink==1 && st.blksize>0 && st.atime.nanoseconds<1000000000U);
    }
    long r=SC5(291,fd,"",0x1000,0x7ff,&st);
    abi_record("statx.fd-empty",r,r ? -1 : (long)st.size,r ? -1 : st.mode&0170000,0,0,0);
    r=SC5(291,-100,"",0x1000,0x7ff,&st);
    abi_record("statx.cwd-empty",r,r ? -1 : (st.mode&0170000)==0040000,-1,0,0,0);
    r=SC5(291,-100,"/statx-link",0x100,0x7ff,&st);
    abi_record("statx.symlink",r,r ? -1 : (long)st.size,r ? -1 : st.mode&0170000,0,0,0);
    r=SC5(291,-100,"/statx-link",0,0x7ff,&st);
    abi_record("statx.follow",r,r ? -1 : (long)st.size,r ? -1 : st.mode&0170000,0,0,0);
    abi_record("statx.reserved",SC5(291,-100,"/statx-data",0,0x80000000U,&st),-1,-1,0,0,0);
    abi_record("statx.invalid-sync",SC5(291,-100,"/statx-data",0x6000,0x7ff,&st),-1,-1,0,0,0);
    abi_record("statx.invalid-flags",SC5(291,fd,"",0x1001,0x7ff,&st),-1,-1,0,0,0);
    abi_record("statx.bad-path",SC5(291,-100,8,0,0x7ff,&st),-1,-1,0,0,0);
    abi_record("statx.bad-buffer",SC5(291,-100,"/statx-data",0,0x7ff,8),-1,-1,0,0,0);
    abi_record("statx.bad-fd",SC5(291,9999,"",0x1000,0x7ff,&st),-1,-1,0,0,0);
    abi_record("statx.empty",SC5(291,-100,"",0,0x7ff,&st),-1,-1,0,0,0);
    r=SC5(291,-100,"/statx-data",0,0x40000000,&st);
    abi_record("statx.unknown-mask",r,r ? -1 : (long)st.size,-1,0,0,0);
    abi_record("statx.null-fd",SC5(291,fd,0,0x1000,0x7ff,&st),-1,-1,0,0,0);
    abi_record("statx.bad-path-mask",SC5(291,-100,8,0,0x80000000U,&st),-1,-1,0,0,0);
    abi_record("statx.bad-path-sync",SC5(291,-100,8,0x6000,0x7ff,&st),-1,-1,0,0,0);
    abi_record("statx.bad-path-flags",SC5(291,-100,8,1,0x7ff,&st),-1,-1,0,0,0);
    abi_record("statx.path-flags",SC5(291,-100,"/statx-data",1,0x7ff,&st),-1,-1,0,0,0);
    unsigned char *partial=(void *)SC6(222,0,(2 * ABI_PAGE_SIZE),3,0x22,-1,0);abi_require((long)partial>0);
    abi_require(SC3(226,partial+ABI_PAGE_SIZE,ABI_PAGE_SIZE,0)==0);
    abi_record("statx.cross-copy",SC5(291,-100,"/statx-data",0,0x7ff,partial+ABI_PAGE_SIZE-128),-1,-1,0,0,0);
    abi_require(SC2(215,partial,(2 * ABI_PAGE_SIZE))==0);
    abi_require(SC1(57,fd)==0 && SC3(35,-100,"/statx-link",0)==0 && SC3(35,-100,"/statx-data",0)==0);
}
