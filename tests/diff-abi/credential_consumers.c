#include "abi.h"
#include <stdint.h>

struct consumer_packet {const char *name;long result;};
struct consumer_perm {int32_t key;uint32_t uid,gid,cuid,cgid,mode;uint16_t seq,pad;uint64_t unused[2];};
struct consumer_shm {struct consumer_perm perm;uint64_t size;int64_t atime,dtime,ctime;int32_t cpid,lpid;uint64_t nattch,reserved[2];};
static long output,parent,root_shm,root_fd,random_fd;
static int root_pipe[2];
#ifdef ABI_CREDENTIAL_CONSUMER_LOOP
static long loop_rd,loop_wr;
#endif
static void result(const char *name,long value)
{struct consumer_packet p={name,value};abi_require(SC3(64,output,&p,sizeof(p))==(long)sizeof(p));}
static long success(long value){return value<0?value:0;}
static void proc_path(char *out,long pid,long fd)
{
    const char *prefix="/proc/";usize at=0;while(*prefix)out[at++]=*prefix++;
    char digits[24];unsigned n=0;do{digits[n++]='0'+pid%10;pid/=10;}while(pid);
    while(n)out[at++]=digits[--n];
    prefix="/fd/";while(*prefix)out[at++]=*prefix++;
    do{digits[n++]='0'+fd%10;fd/=10;}while(fd);while(n)out[at++]=digits[--n];out[at]=0;
}
static int contains(const char *data,usize length,const char *wanted)
{
    for(usize i=0;i<length;i++) {
        usize j=0;while(wanted[j] && i+j<length && data[i+j]==wanted[j])j++;
        if(!wanted[j])return 1;
    }
    return 0;
}
static void drop(void)
{unsigned groups[]={2000};abi_require(!SC2(159,1,groups)&&!SC1(144,1000)&&!SC1(146,1000));}
static void restricted(void)
{
    drop();
    struct abi_stat st;abi_require(!SC4(79,-100,"/proc/self",&st,0));
    result("cred-use.proc-dir-owner",st.uid==1000&&st.gid==1000?0:-1);
    abi_require(!SC4(79,-100,"/proc/self/status",&st,0));
    result("cred-use.proc-nondumpable-file-owner",!st.uid&&!st.gid?0:-1);
    long statusfd=abi_open("/proc/self/status",0);abi_require(statusfd>=0);char status_text[2048];
    long bytes=SC3(63,statusfd,status_text,sizeof(status_text));abi_require(bytes>=0);SC1(57,statusfd);
    result("cred-use.proc-identity-fields",contains(status_text,(usize)bytes,"Uid:\t1000\t1000\t1000\t1000\n")&&
        contains(status_text,(usize)bytes,"Gid:\t1000\t1000\t1000\t1000\n")&&
        contains(status_text,(usize)bytes,"Groups:\t2000 \n")?0:-1);
    result("cred-use.proc-capability-fields",contains(status_text,(usize)bytes,"CapEff:\t0000000000000000\n")&&
        contains(status_text,(usize)bytes,"CapPrm:\t0000000000000000\n")&&
        contains(status_text,(usize)bytes,"NoNewPrivs:\t0\n")?0:-1);
    result("cred-use.kill-self-zero",SC2(129,SC0(172),0));
    result("cred-use.kill-other-zero",SC2(129,parent,0));
    result("cred-use.kill-other-standard",SC2(129,parent,10));
    unsigned char info[128]={0};*(int *)(info+8)=-1;*(uint32_t *)(info+16)=(uint32_t)SC0(172);*(uint32_t *)(info+20)=1000;
    result("cred-use.queue-other",SC3(138,parent,34,info));
    uint64_t head=0,length=0;result("cred-use.robust-other",SC3(100,parent,&head,&length));
    result("cred-use.robust-self",SC3(100,0,&head,&length));
    char path[80],data[64];proc_path(path,parent,root_fd);
    result("cred-use.proc-other-readlink",success(SC4(78,-100,path,data,sizeof(data))));
    result("cred-use.proc-other-reopen",success(abi_open(path,0)));
    proc_path(path,SC0(172),root_fd);result("cred-use.proc-self-readlink",success(SC4(78,-100,path,data,sizeof(data))));
    char byte=0;abi_require(SC3(64,root_pipe[1],"p",1)==1);
    result("cred-use.pipe-old-open-rights",SC3(63,root_pipe[0],&byte,1)==1&&byte=='p'?0:-1);
    proc_path(path,SC0(172),root_pipe[0]);long reopened=abi_open(path,0);
    result("cred-use.pipe-private-reopen",success(reopened));if(reopened>=0)SC1(57,reopened);
    uint64_t limit[2]={0};result("cred-use.prlimit-other",SC4(261,parent,4,0,limit));
    result("cred-use.prlimit-invalid-other",SC4(261,parent,99,0,limit));
    uint64_t replacement[2]={1,1};abi_require(!SC4(261,0,1,replacement,0));replacement[1]=2;
    result("cred-use.prlimit-raise-hard",SC4(261,0,1,replacement,0));
    uint64_t mask=1;result("cred-use.affinity-other",SC3(122,parent,8,&mask));
    int priority=1;result("cred-use.sched-rt",SC3(119,0,1,&priority));
    result("cred-use.mount",SC5(40,0,"/","tmpfs",0,0));
    result("cred-use.mount-bad-input",SC5(40,0,"/",-1,0,0));
    result("cred-use.umount",SC2(39,"/",0));
    result("cred-use.syslog-clear",SC3(116,5,0,0));
    result("cred-use.syslog-zero-read",SC3(116,2,data,0));
    result("cred-use.entropy-inject",SC3(29,random_fd,0x40085203U,-1));
    struct consumer_shm ds={0};result("cred-use.shm-group-stat",SC3(195,root_shm,2,&ds));
    result("cred-use.shm-group-find",success(SC3(194,0x717171,0,0400)));
    result("cred-use.shm-group-write-find",success(SC3(194,0x717171,0,0200)));
    long address=SC3(196,root_shm,0,010000);result("cred-use.shm-group-read",success(address));if(address>=0)abi_require(!SC1(197,address));
    address=SC3(196,root_shm,0,0);result("cred-use.shm-group-write",success(address));if(address>=0)SC1(197,address);
    result("cred-use.shm-remove-other",SC3(195,root_shm,0,0));
    result("cred-use.shm-set-bad-before-owner",SC3(195,root_shm,1,-1));
    result("cred-use.shm-set-other",SC3(195,root_shm,1,&ds));
    result("cred-use.shm-negative-before-input",SC3(195,-1,1,-1));
    result("cred-use.shm-input-before-id",SC3(195,0x7fffffff,1,-1));
    long own=SC3(194,0,ABI_PAGE_SIZE,0600);abi_require(own>=0);abi_require(!SC3(195,own,2,&ds));
    result("cred-use.shm-creator",ds.perm.uid==1000&&ds.perm.gid==1000&&ds.perm.cuid==1000&&ds.perm.cgid==1000?0:-1);
    ds.perm.uid=1001;ds.perm.gid=3000;result("cred-use.shm-owner-reassign",SC3(195,own,1,&ds));
    result("cred-use.shm-creator-remove",SC3(195,own,0,0));
    long sock=SC3(198,2,2,0);abi_require(sock>=0);
    struct {uint16_t family,port;uint32_t address;uint64_t zero;} addr={2,0x5000,0,0};
    result("cred-use.bind-low-port",SC3(200,sock,&addr,sizeof(addr)));addr.port=0;
    result("cred-use.bind-ephemeral",SC3(200,sock,&addr,sizeof(addr)));
    struct {char name[16];unsigned char data[24];} ifr={{'l','o',0},{0}};
    result("cred-use.net-config",SC3(29,sock,0x8914,&ifr));SC1(57,sock);
}
#ifdef ABI_CREDENTIAL_CONSUMER_LOOP
static void loop_restricted(void)
{
    drop();
    result("cred-use.blk-roset",SC3(29,loop_rd,0x125d,-1));
    result("cred-use.blk-bszset",SC3(29,loop_rd,0x40081271,0));
    result("cred-use.blk-flush",SC3(29,loop_rd,0x1261,0));
    result("cred-use.loop-status-readonly",SC3(29,loop_rd,0x4c04,-1));
    result("cred-use.loop-capacity-readonly",SC3(29,loop_rd,0x4c07,0));
    result("cred-use.loop-capacity-write",SC3(29,loop_wr,0x4c07,0));
 }
#endif
static void same_user(void)
{
    drop();abi_require(!SC2(167,4,1));
    int control[2];abi_require(!SC2(59,control,0));long child=SC5(220,17,0,0,0,0);abi_require(child>=0);
    if(!child){SC1(57,control[1]);char c;abi_require(SC3(63,control[0],&c,1)==1);abi_exit(0);}
    SC1(57,control[0]);char path[80],buf[32];proc_path(path,child,root_fd);
    result("cred-use.same-user-kill",SC2(129,child,0));
    uint64_t head=0,length=0;result("cred-use.same-user-robust",SC3(100,child,&head,&length));
    result("cred-use.same-user-proc",success(SC4(78,-100,path,buf,sizeof(buf))));
    abi_require(SC3(64,control[1],"x",1)==1);SC1(57,control[1]);int status;
    abi_require(SC4(260,child,&status,0,0)==child&&!status);
}
static void pipe_public(void)
{
    drop();char path[80];proc_path(path,SC0(172),root_pipe[0]);long fd=abi_open(path,0);
    result("cred-use.pipe-public-reopen",success(fd));if(fd>=0)SC1(57,fd);
}
static void syslog_caps(void)
{
    struct {unsigned version;int pid;} h={0x20080522,0};
    struct {unsigned effective,permitted,inheritable;} caps[2];abi_require(!SC2(90,&h,caps));
    caps[0].effective=1U<<21;caps[1].effective=0;abi_require(!SC2(91,&h,caps));
    result("cred-use.syslog-admin-only",SC3(116,5,0,0));
    caps[0].effective=0;caps[1].effective=1U<<(34-32);abi_require(!SC2(91,&h,caps));
    result("cred-use.syslog-syslog-cap",SC3(116,5,0,0));
}
static void access_override(void)
{
    abi_require(!SC3(149,1000,0,0)&&!SC3(147,1000,0,0));
    char path[80];proc_path(path,parent,0);
    usize at=6;while(path[at] && path[at]!='/')at++;
    const char *tail="/exe";while(*tail)path[at++]=*tail++;path[at]=0;
    result("cred-use.access-proc-real-credentials",SC3(48,-100,path,4));
}
static void isolated(void (*function)(void))
{
    int pipe[2];abi_require(!SC2(59,pipe,0));long child=SC5(220,17,0,0,0,0);abi_require(child>=0);
    if(!child){SC1(57,pipe[0]);output=pipe[1];function();abi_exit(0);}
    SC1(57,pipe[1]);
    for(;;){struct consumer_packet packet;usize got=0;while(got<sizeof(packet)){long n=SC3(63,pipe[0],(char *)&packet+got,sizeof(packet)-got);abi_require(n>=0);if(!n)break;got+=(usize)n;}
        if(!got)break;
        abi_require(got==sizeof(packet));abi_record(packet.name,packet.result,-1,-1,0,0,0);}
    SC1(57,pipe[0]);int status;abi_require(SC4(260,child,&status,0,0)==child&&!status);
}

#ifdef ABI_CREDENTIAL_SIGNAL_TARGET
extern long abi_clone_entry(long,void *,int *,void (*)(void *),void *);
static unsigned char target_stack[16384] __attribute__((aligned(16)));
static int target_ready[2],target_release[2],member_ready[2],member_release[2];
static volatile int member_clear;
static long target_leader,target_member,actor_uid;
static void token_write(int fd){abi_require(SC3(64,fd,"x",1)==1);}
static void token_read(int fd){char byte;abi_require(SC3(63,fd,&byte,1)==1);}
static void target_worker(void *unused)
{
    (void)unused;abi_require(!SC1(146,1001));
    token_write(member_ready[1]);token_read(member_release[0]);
}
static void target_sender(void)
{
    abi_require(!SC1(146,actor_uid));
    result(actor_uid==1000?"cred-target.other-member-probe":"cred-target.own-member-probe",SC2(129,target_member,0));
    result(actor_uid==1000?"cred-target.other-member-kill":"cred-target.own-member-kill",SC2(129,target_member,10));
    unsigned char info[128]={0};*(int *)(info+8)=-1;
    *(uint32_t *)(info+16)=(uint32_t)SC0(172);*(uint32_t *)(info+20)=(uint32_t)actor_uid;
    result(actor_uid==1000?"cred-target.other-member-queue":"cred-target.own-member-queue",SC3(138,target_member,34,info));
    result(actor_uid==1000?"cred-target.own-leader-probe":"cred-target.other-leader-probe",SC2(129,target_leader,0));
}
static usize append_pid(char *text,usize at,long pid)
{
    char digits[24];unsigned count=0;do{digits[count++]='0'+pid%10;pid/=10;}while(pid);
    while(count)text[at++]=digits[--count];
    return at;
}
static long target_queued(long member)
{
    char path[96]="/proc/";usize at=append_pid(path,6,target_leader);
    if(member){const char *part="/task/";while(*part)path[at++]=*part++;at=append_pid(path,at,member);}
    const char *part="/status";while(*part)path[at++]=*part++;path[at]=0;
    long fd=abi_open(path,0);abi_require(fd>=0);char text[2048];long n=SC3(63,fd,text,sizeof(text));abi_require(n>0);SC1(57,fd);
    for(long i=0;i+6<n;i++)if(text[i]=='S'&&text[i+1]=='i'&&text[i+2]=='g'&&text[i+3]=='Q'&&text[i+4]==':') {
        i+=5;while(i<n&&(text[i]=='\t'||text[i]==' '))i++;
        long value=0;while(i<n&&text[i]>='0'&&text[i]<='9')value=value*10+text[i++]-'0';return value;
    }
    abi_require(0);return -1;
}
static void signal_target_case(void)
{
    abi_require(!SC3(34,-100,"/proc",0555)&&!SC5(40,0,"/proc","proc",0,0));
    abi_require(!SC2(59,target_ready,0)&&!SC2(59,target_release,0)&&!SC2(59,member_ready,0)&&!SC2(59,member_release,0));
    uint64_t mask=(UINT64_C(1)<<9)|(UINT64_C(1)<<33);abi_require(!SC4(135,0,&mask,0,8));
    target_leader=SC5(220,17,0,0,0,0);abi_require(target_leader>=0);
    if(!target_leader) {
        long member=abi_clone_entry(0x1210f00,target_stack+sizeof(target_stack),(int *)&member_clear,target_worker,0);
        abi_require(member>0);token_read(member_ready[0]);abi_require(!SC1(146,1000));
        long report[2]={member,SC2(129,member,0)};
        abi_require(SC3(64,target_ready[1],report,sizeof(report))==(long)sizeof(report));
        token_read(target_release[0]);token_write(member_release[1]);
        struct {long sec,ns;} timeout={2,0};
        while(__atomic_load_n(&member_clear,__ATOMIC_ACQUIRE)) {
            int value=__atomic_load_n(&member_clear,__ATOMIC_ACQUIRE);if(!value)break;
            long r=SC4(98,&member_clear,0,value,&timeout);abi_require(!r||r==-11||r==-4);
        }
        abi_exit(0);
    }
    long report[2];abi_require(SC3(63,target_ready[0],report,sizeof(report))==(long)sizeof(report));
    target_member=report[0];abi_record("cred-target.same-group-different-uid",report[1],-1,-1,0,0,0);
    actor_uid=1000;isolated(target_sender);actor_uid=1001;isolated(target_sender);
    abi_record("cred-target.leader-account",target_queued(0),-1,-1,0,0,0);
    abi_record("cred-target.member-account",target_queued(target_member),-1,-1,0,0,0);
    token_write(target_release[1]);int status;abi_require(SC4(260,target_leader,&status,0,0)==target_leader&&!status);
    for(unsigned i=0;i<2;i++){SC1(57,target_ready[i]);SC1(57,target_release[i]);SC1(57,member_ready[i]);SC1(57,member_release[i]);}
    long child=SC5(220,17,0,0,0,0);abi_require(child>=0);
    if(!child){abi_require(!SC1(146,1001));abi_exit(7);}
    unsigned char info[128]={0};long r=SC5(95,1,child,info,4|0x1000000,0);
    abi_record("cred-target.waitid-peek-uid",r?r:*(uint32_t *)(info+20),-1,-1,0,0,0);
    abi_record("cred-target.waitid-peek-status",*(int32_t *)(info+24),-1,-1,0,0,0);
    r=SC5(95,1,child,info,4,0);
    abi_record("cred-target.waitid-consume-uid",r?r:*(uint32_t *)(info+20),-1,-1,0,0,0);
    abi_record("cred-target.waitid-consume-status",*(int32_t *)(info+24),-1,-1,0,0,0);
    abi_require(!SC2(39,"/proc",0)&&!SC3(35,-100,"/proc",0x200));
}
#endif

void abi_credential_consumer_cases(void)
{
#ifdef ABI_CREDENTIAL_SIGNAL_TARGET
    signal_target_case();
#elif defined(ABI_CREDENTIAL_ACCESS_OVERRIDE)
    abi_require(!SC3(34,-100,"/proc",0555)&&!SC5(40,0,"/proc","proc",0,0));
    parent=SC0(172);isolated(access_override);
    abi_require(!SC2(39,"/proc",0)&&!SC3(35,-100,"/proc",0x200));
#elif defined(ABI_CREDENTIAL_CONSUMER_LOOP)
    abi_require(!SC4(33,-100,"/dev/cred-loop",060666,7U<<8));
    loop_rd=abi_open("/dev/cred-loop",0);loop_wr=abi_open("/dev/cred-loop",2);
    abi_require(loop_rd>=0&&loop_wr>=0);isolated(loop_restricted);
    SC1(57,loop_rd);SC1(57,loop_wr);abi_require(!SC3(35,-100,"/dev/cred-loop",0));
#else
    abi_require(!SC3(34,-100,"/proc",0555));
    abi_require(!SC5(40,0,"/proc","proc",0,0));
    parent=SC0(172);root_fd=abi_open("/data",0);abi_require(root_fd>=0);
    abi_require(!SC2(59,root_pipe,0));
    uint64_t mask=(UINT64_C(1)<<9)|(UINT64_C(1)<<33);abi_require(!SC4(135,0,&mask,0,8));
    abi_require(!SC4(33,-100,"/dev/cred-random",020666,(1U<<8)|8));
    random_fd=abi_open("/dev/cred-random",0);abi_require(random_fd>=0);
    root_shm=SC3(194,0x717171,ABI_PAGE_SIZE,01000|0600);abi_require(root_shm>=0);
    struct consumer_shm ds={0};abi_require(!SC3(195,root_shm,2,&ds));ds.perm.gid=2000;ds.perm.mode=0640;abi_require(!SC3(195,root_shm,1,&ds));
    isolated(restricted);isolated(same_user);
    abi_require(!SC2(52,root_pipe[0],0666));isolated(pipe_public);isolated(syslog_caps);
    SC1(57,root_pipe[0]);SC1(57,root_pipe[1]);
    abi_require(!SC3(195,root_shm,0,0));SC1(57,random_fd);SC1(57,root_fd);
    abi_require(!SC3(35,-100,"/dev/cred-random",0));
    abi_require(!SC2(39,"/proc",0)&&!SC3(35,-100,"/proc",0x200));
#endif
}
