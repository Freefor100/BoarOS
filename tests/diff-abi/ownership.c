#include "abi.h"

static void owner_record(const char *prefix, const char *name, long result,
                         long fd, const struct abi_stat *saved)
{
    char id[80]; unsigned n=0;
    while (*prefix) id[n++]=*prefix++;
    while (*name) id[n++]=*name++;
    id[n]=0;
    struct abi_stat st;
    if (saved) st=*saved;
    else abi_require(SC2(80,fd,&st)==0);
    abi_record(id,result,st.uid,st.gid,0,&st.mode,sizeof(st.mode));
}

static void owner_files(const char *directory, const char *prefix)
{
    abi_require(SC3(34,-100,directory,0700)==0);
    long dir=abi_open(directory,0200000),fd=SC4(56,dir,"file",0100|2,0600);
    abi_require(dir>=0 && fd>=0 && SC2(52,fd,06755)==0);
    struct abi_stat before,after;
    abi_require(SC2(80,fd,&before)==0);
    long pause[2]={0,20000000};abi_require(SC2(101,pause,0)==0);
    long r=SC3(55,fd,70001,80002);
    owner_record(prefix,"high-ids",r,fd,0);
    abi_require(SC2(80,fd,&after)==0);
    char id[80];unsigned n=0;while(prefix[n]){id[n]=prefix[n];n++;}
    const char *suffix="times";while(*suffix)id[n++]=*suffix++;id[n]=0;
    long changed[3]={before.ctime!=after.ctime || before.ctime_nsec!=after.ctime_nsec,
        before.atime==after.atime && before.atime_nsec==after.atime_nsec,
        before.mtime==after.mtime && before.mtime_nsec==after.mtime_nsec};
    abi_record(id,0,-1,-1,0,changed,sizeof(changed));
    r=SC3(55,fd,-1,80003);owner_record(prefix,"keep-uid",r,fd,0);
    abi_require(SC2(52,fd,06640)==0);
    r=SC3(55,fd,-1,-1);owner_record(prefix,"keep-ids-drop-suid",r,fd,0);
    r=SC3(55,fd,0xfffffffeUL,1UL<<32|17);owner_record(prefix,"cast-ids",r,fd,0);
    abi_require(SC5(37,dir,"file",dir,"hard",0)==0);
    long other=SC4(56,dir,"hard",0,0);abi_require(other>=0);
    r=SC3(55,other,123,456);owner_record(prefix,"hardlink",r,fd,0);
    abi_require(SC1(57,other)==0 && SC3(36,"file",dir,"sym")==0);
    r=SC5(54,dir,"sym",321,654,0);owner_record(prefix,"follow-link",r,fd,0);
    r=SC5(54,dir,"sym",111,222,0x100);
    abi_require(SC4(79,dir,"sym",&after,0x100)==0);
    owner_record(prefix,"nofollow-link",r,0,&after);
    owner_record(prefix,"nofollow-target",0,fd,0);
    r=SC5(54,fd,"",333,444,0x1000);owner_record(prefix,"empty-fd",r,fd,0);
    r=SC5(54,fd,0,555,666,0x1000);owner_record(prefix,"null-empty-fd",r,fd,0);
    abi_require(SC3(33,dir,"fifo",0010000|0600)==0);
    long fifo=SC4(56,dir,"fifo",2,0);abi_require(fifo>=0);
    r=SC3(55,fifo,999,888);owner_record(prefix,"fifo",r,fifo,0);
    abi_require(SC1(57,fifo)==0 && SC3(35,dir,"fifo",0)==0);
    abi_require(SC2(52,dir,06755)==0);
    r=SC3(55,dir,101,202);owner_record(prefix,"directory-setid",r,dir,0);
    other=SC4(56,dir,"inherit",0100|2,0600);abi_require(other>=0);
    owner_record(prefix,"inherit-file",0,other,0);
    abi_require(SC1(57,other)==0 && SC3(34,dir,"inherit-dir",0700)==0);
    other=SC4(56,dir,"inherit-dir",0200000,0);abi_require(other>=0);
    owner_record(prefix,"inherit-directory",0,other,0);
    abi_require(SC1(57,other)==0 && SC3(35,dir,"inherit",0)==0 && SC3(35,dir,"inherit-dir",0x200)==0);
    abi_require(SC3(35,dir,"sym",0)==0 && SC3(35,dir,"hard",0)==0 && SC3(35,dir,"file",0)==0);
    r=SC3(55,fd,777,888);owner_record(prefix,"unlinked-fd",r,fd,0);
    r=SC5(54,fd,"",778,889,0x1000);owner_record(prefix,"unlinked-empty",r,fd,0);
    abi_require(SC1(82,dir)==0 && SC1(57,fd)==0 && SC1(57,dir)==0);
    abi_require(SC3(35,-100,directory,0x200)==0);
}

void abi_ownership_cases(void)
{
    owner_files("/owner-ext4","owner.ext4.");
    abi_require(SC3(34,-100,"/owner-tmp",0700)==0 && SC5(40,"tmpfs","/owner-tmp","tmpfs",0,0)==0);
    owner_files("/owner-tmp/dir","owner.tmpfs.");
    abi_require(SC2(39,"/owner-tmp",0)==0);
    abi_require(SC5(40,"tmpfs","/owner-tmp","tmpfs",1,0)==0);
    long fd=abi_open("/owner-tmp",0200000);abi_require(fd>=0);
    owner_record("owner.","readonly",SC3(55,fd,-1,-1),fd,0);
    long closed=SC1(57,fd),unmounted=SC2(39,"/owner-tmp",0);
    abi_record("owner.readonly-close",closed,-1,-1,0,0,0);
    abi_record("owner.unmount",unmounted,-1,-1,0,0,0);
    abi_require(closed==0 && unmounted==0 && SC3(35,-100,"/owner-tmp",0x200)==0);
    long commands[][5]={{-1,0,0,0,0},{-1,1,0,0,4},{-1,1,0,0,0},
        {-1,(long)"file",0,0,0},{-100,(long)"/no-owner-file",0,0,0},
        {-1,(long)"",0,0,0x1000},{-1,(long)"",0,0,0},{-1,0,0,0,0x1000}};
    const char *names[]={"owner.bad-fd","owner.bad-flags","owner.bad-pointer",
        "owner.bad-dirfd","owner.missing","owner.empty-bad-fd",
        "owner.empty-no-flag","owner.null-empty-bad-fd"};
    for(unsigned i=0;i<8;i++) abi_record(names[i],i==0?SC3(55,-1,0,0):
        SC5(54,commands[i][0],commands[i][1],commands[i][2],commands[i][3],commands[i][4]),-1,-1,0,0,0);
    int pipes[2];abi_require(SC2(59,pipes,0)==0);
    owner_record("owner.","pipe",SC3(55,pipes[0],1234,5678),pipes[1],0);
    owner_record("owner.","pipe-empty",SC5(54,pipes[1],"",2345,6789,0x1000),pipes[0],0);
    abi_require(SC1(57,pipes[0])==0 && SC1(57,pipes[1])==0);
}
