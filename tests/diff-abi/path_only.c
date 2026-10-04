#include "abi.h"

static void record(const char *id, long ret) { abi_record(id,ret,-1,-1,0,0,0); }
void abi_path_only_cases(void)
{
    abi_require(SC3(34,-100,"/path-proc",0755)==0 && SC5(40,"proc","/path-proc","proc",0,0)==0);
    long fd=abi_open("/path-target",2|64|128); abi_require(fd>=0);
    abi_require(SC3(64,fd,"data",4)==4);
    long path=abi_open("/path-target",010000000|02000000|2|01000|0100);
    record("path.open",path<0?path:0);
    if(path<0) { SC1(57,fd); return; }
    record("path.getfl",SC3(25,path,3,0));
    record("path.getfd",SC3(25,path,1,0));
    record("path.read-zero",SC3(63,path,-1,0));
    record("path.write-zero",SC3(64,path,-1,0));
    record("path.seek",SC3(62,path,0,0));
    record("path.chmod",SC2(52,path,0600));
    record("path.chown",SC3(55,path,0,0));
    record("path.truncate",SC2(46,path,0));
    record("path.ioctl",SC3(29,path,0,-1));
    record("path.interface-ioctl",SC3(29,path,0x8913,-1));
    int option=0;unsigned option_length=sizeof(option);
    record("path.getsockopt",SC5(209,path,1,3,&option,&option_length));
    long read_bits=1UL<<path,write_bits=read_bits,error_bits=read_bits;
    long select_timeout[2]={0,0};
    long selected=SC6(72,path+1,&read_bits,&write_bits,&error_bits,select_timeout,0);
    long select_bits[3]={read_bits,write_bits,error_bits};
    abi_record("path.select",selected,-1,-1,0,select_bits,sizeof(select_bits));
    struct { unsigned events; unsigned long data; } __attribute__((packed)) event={1,0};
    record("path.epoll-control",SC4(21,path,1,fd,&event));
    record("path.epoll-wait",SC6(22,path,&event,1,0,0,0));
    record("path.setfl",SC3(25,path,4,0));
    record("path.lock-fault",SC3(25,path,5,-1));
    record("path.sync",SC1(82,path));
    record("path.mmap",SC6(222,0,4096,1,2,path,0));
    struct abi_stat st; long r=SC2(80,path,&st);
    abi_record("path.stat",r,st.size,-1,0,&st.mode,sizeof(st.mode));
    record("path.empty-owner",SC5(54,path,"",123,456,0x1000));
    r=SC4(79,path,"",&st,0x1000);
    abi_record("path.empty-stat",r,st.size,-1,0,&st.uid,sizeof(st.uid));
    long dir=abi_open("/",010000000|0200000);
    record("path.dir-open",dir<0?dir:0);
    long child=SC4(56,dir,"path-target",010000000,0);
    record("path.relative",child<0?child:0); if(child>=0) SC1(57,child);
    record("path.fchdir",SC1(50,dir));
    record("path.getdents",SC3(61,dir,-1,0));
    abi_require(SC3(36,"path-target",dir,"path-sym")==0);
    long sym=abi_open("/path-sym",010000000|0400000);
    record("path.symlink-open",sym<0?sym:0);
    char data[64];for(unsigned i=0;i<sizeof(data);i++)data[i]=0;
    r=SC4(78,sym,"",data,sizeof(data));
    abi_record("path.empty-readlink",r,-1,-1,0,data,r>0?(usize)r:0);
    record("path.empty-link",SC5(37,sym,"",dir,"path-hard",0x1000));
    record("path.symlink-directory",abi_open("/path-sym",010000000|0400000|0200000));
    abi_require(SC4(33,dir,"path-fifo",010600,0)==0);
    long fifo=abi_open("/path-fifo",010000000|1);
    record("path.fifo-open",fifo<0?fifo:0);if(fifo>=0)SC1(57,fifo);
    long device=abi_open("/dev/rtc0",010000000);
    record("path.device-open",device<0?device:0);if(device>=0)SC1(57,device);
    long dup=SC1(23,path); record("path.dup",dup<0?dup:0);
    char proc_live[48];const char *base="/path-proc/self/fd/";
    unsigned live_pos=0;while(base[live_pos]){proc_live[live_pos]=base[live_pos];live_pos++;}
    unsigned live_num=(unsigned)path;char live_digits[12];unsigned live_used=0;
    do {live_digits[live_used++]=(char)('0'+live_num%10);live_num/=10;}while(live_num);
    while(live_used) proc_live[live_pos++]=live_digits[--live_used];
    proc_live[live_pos]=0;
    long reopened=abi_open(proc_live,0);record("path.proc-live-reopen",reopened<0?reopened:0);
    if(reopened>=0){char bytes[4];long count=SC3(63,reopened,bytes,sizeof(bytes));abi_record("path.proc-live-content",count,-1,-1,0,bytes,count>0?(usize)count:0);SC1(57,reopened);}
    else abi_record("path.proc-live-content",reopened,-1,-1,0,0,0);
    struct abi_stat followed;long followed_result=SC4(79,-100,proc_live,&followed,0);
    abi_record("path.proc-live-stat",followed_result,followed_result? -1:followed.size,-1,0,0,0);
    long fsinfo[15];long fsresult=SC2(44,path,fsinfo);
    abi_record("path.statfs",fsresult,-1,-1,0,fsresult?0:fsinfo,fsresult?0:2*sizeof(long));
    abi_require(SC3(35,dir,"path-target",0)==0);
    long replacement=abi_open("/path-target",2|64|128);abi_require(replacement>=0);
    struct abi_stat newer; abi_require(SC2(80,path,&st)==0 && SC2(80,replacement,&newer)==0);
    long same=st.ino==newer.ino;abi_record("path.recreate-identity",0,st.size,-1,0,&same,sizeof(same));
    struct { int fd; short events,revents; } pollfd={(int)path,5,0};
    long timeout[2]={0,0};r=SC5(73,&pollfd,1,timeout,0,0);
    abi_record("path.poll",r,-1,-1,0,&pollfd.revents,sizeof(pollfd.revents));
    record("path.readv-fault",SC3(65,path,-1,1));
    record("path.utime-direct",SC4(88,path,0,0,0));
    record("path.utime-empty",SC4(88,path,"",0,0x1000));
    long odd=abi_open("/path-target",010000000|3|(1L<<31));
    record("path.invalid-flags",odd<0?odd:0);if(odd>=0)SC1(57,odd);
    long no_create=abi_open("/path-absent",010000000|0100|0200);
    record("path.ignore-create",no_create);if(no_create>=0)SC1(57,no_create);
    record("path.empty",abi_open("",010000000));
    record("path.bad-input",abi_open((void *)-1L,010000000));
    char proc[40];
    const char *prefix="/proc/self/fd/";
    unsigned pos=0;while(prefix[pos]){proc[pos]=prefix[pos];pos++;}unsigned n=(unsigned)path;
    char digits[12];unsigned used=0;do {digits[used++]=(char)('0'+n%10);n/=10;}while(n);
    while(used) proc[pos++]=digits[--used];
    proc[pos]=0;
    long reopen=abi_open(proc,0);record("path.proc-reopen",reopen<0?reopen:0);
    if(reopen>=0)SC1(57,reopen);
    long pid=SC5(220,17,0,0,0,0);abi_require(pid>=0);
    if(!pid){struct abi_stat child_st;abi_exit(SC2(80,path,&child_st)?99:0);}
    int status=0;abi_require(SC4(260,pid,&status,0,0)==pid);
    abi_record("path.fork",0,-1,-1,status,0,0);
    unsigned long old_limit[2],low_limit[2];
    abi_require(SC4(261,0,7,0,old_limit)==0);
    low_limit[0]=0;low_limit[1]=old_limit[1];abi_require(SC4(261,0,7,low_limit,0)==0);
    record("path.full-bad-input",abi_open((void *)-1L,010000000));
    record("path.full-valid",abi_open("/path-target",010000000));
    record("path.full-bad-dirfd",SC4(56,-1,"path-target",010000000,0));
    abi_require(SC4(261,0,7,old_limit,0)==0);
    SC1(57,replacement);SC1(57,dup);SC1(57,sym);SC1(57,dir);SC1(57,path);SC1(57,fd);
    abi_require(SC2(39,"/path-proc",0)==0 && SC3(35,-100,"/path-proc",0x200)==0);
}

void abi_path_truncate_cases(void)
{
    long fd=abi_open("/path-truncate",2|64|128);abi_require(fd>=0);
    abi_require(SC3(64,fd,"abcdefgh",8)==8 && SC2(52,fd,06755)==0);
    record("truncate.path-negative-fault",SC2(45,-1,-1));
    record("truncate.path-fault",SC2(45,-1,0));
    record("truncate.path-missing",SC2(45,"/no-path-truncate",0));
    record("truncate.path-directory",SC2(45,"/",0));
    record("truncate.path-device",SC2(45,"/dev/null",0));
    record("truncate.path-empty",SC2(45,"",0));
    abi_require(SC3(36,"/path-truncate",-100,"/path-truncate-sym")==0);
    long r=SC2(45,"/path-truncate-sym",4);struct abi_stat st;
    abi_require(SC2(80,fd,&st)==0);
    abi_record("truncate.path-symlink",r,st.size,-1,0,&st.mode,sizeof(st.mode));
    r=SC2(45,"path-truncate",4);abi_record("truncate.path-same",r,abi_size(fd),-1,0,0,0);
    r=SC2(45,"/path-truncate",8193);abi_record("truncate.path-extend",r,abi_size(fd),-1,0,0,0);
    char tail[8];long got=SC4(67,fd,tail,sizeof(tail),8190);
    abi_record("truncate.path-tail",got,abi_size(fd),-1,0,tail,got>0?(usize)got:0);
    r=SC2(45,"/path-truncate",0);abi_record("truncate.path-zero",r,abi_size(fd),-1,0,0,0);
    record("truncate.path-running-executable",SC2(45,"/init",0));
    SC1(57,fd);SC3(35,-100,"/path-truncate",0);SC3(35,-100,"/path-truncate-sym",0);
}
