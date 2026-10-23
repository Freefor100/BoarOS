#include "abi.h"

struct credential_packet { const char *name; long result; usize size; unsigned char data[128]; };
static int output;
static void record(const char *name,long result,const void *data,usize size)
{
    struct credential_packet p={.name=name,.result=result,.size=size};
    abi_require(size<=sizeof(p.data));
    for(usize i=0;i<size;i++)p.data[i]=((const unsigned char *)data)[i];
    abi_require(SC3(64,output,&p,sizeof(p))==(long)sizeof(p));
}
static void state(const char *name,long result)
{
    unsigned v[8];
    abi_require(!SC3(148,&v[0],&v[1],&v[2])&&!SC3(150,&v[4],&v[5],&v[6]));
    v[3]=(unsigned)SC1(151,-1);v[7]=(unsigned)SC1(152,-1);
    record(name,result,v,sizeof(v));
}
struct cred_header{unsigned version;int pid;};
struct cred_data{unsigned effective,permitted,inheritable;};
static void capabilities(const char *name,long result)
{
    struct cred_header h={0x20080522,0};struct cred_data data[2]={{0}};
    abi_require(!SC2(90,&h,data));record(name,result,data,sizeof(data));
}
static void ids(void)
{
    state("cred.initial",0);
    record("cred.setuid-invalid",SC1(146,-1),0,0);
    record("cred.setgid-invalid",SC1(144,-1),0,0);
    state("cred.res-noop",SC3(147,-1,-1,-1));
    state("cred.gid-res-root",SC3(149,2000,2001,2002));
    state("cred.gid-re-root",SC2(143,2003,-1));
    state("cred.fsgid-root",SC1(152,2004));
    state("cred.uid-res-drop",SC3(147,1000,1001,1002));
    capabilities("cred.uid-drops-caps",0);
    state("cred.uid-res-swap",SC3(147,1001,1002,1000));
    state("cred.uid-real-rejects-saved",SC2(145,1000,-1));
    state("cred.uid-set-saved",SC1(146,1000));
    state("cred.uid-no-raise",SC1(146,0));
    state("cred.fsuid-unrelated",SC1(151,1234));
    state("cred.fsuid-real",SC1(151,1001));
    state("cred.gid-res-member",SC3(149,2001,2003,2003));
    state("cred.gid-no-raise",SC1(144,0));
    record("cred.groups-unpriv-before-size",SC2(159,-1,-1),0,0);
    unsigned values[3]={11,22,33};long r=SC3(148,&values[0],(void *)-1,&values[2]);
    record("cred.getres-prefix",r,values,sizeof(values));
    r=SC3(150,&values[0],&values[0],&values[0]);record("cred.getres-alias",r,values,sizeof(values));
}
static void groups(void)
{
    unsigned input[]={9,2,9,7},result[6]={11,22,33,44,55,66};
    record("cred.groups-default",SC2(158,0,-1),0,0);
    record("cred.groups-set",SC2(159,4,input),0,0);
    record("cred.groups-size",SC2(158,0,0),0,0);
    record("cred.groups-small",SC2(158,3,-1),0,0);
    long r=SC2(158,6,result);record("cred.groups-sorted",r,result,sizeof(result));
    record("cred.groups-negative",SC2(158,-1,-1),0,0);
    record("cred.groups-too-many",SC2(159,65537,-1),0,0);
    record("cred.groups-input-fault",SC2(159,1,-1),0,0);
    input[0]=~0U;record("cred.groups-invalid-id",SC2(159,1,input),0,0);
    record("cred.groups-output-fault",SC2(158,4,-1),0,0);
    record("cred.groups-empty",SC2(159,0,-1),0,0);
    record("cred.groups-empty-output",SC2(158,4,-1),0,0);
}
static void caps(void)
{
    struct cred_header h={0x20080522,0};struct cred_data data[2]={{0}};
    abi_require(!SC2(90,&h,data));
    h.version=0;long r=SC2(91,&h,0);record("cred.capset-negotiate",r,&h.version,4);
    h.pid=-1;record("cred.capset-target-before-data",SC2(91,&h,-1),0,0);
    h.pid=0;record("cred.capset-input-fault",SC2(91,&h,-1),0,0);
    h.pid=(int)SC0(178);record("cred.capset-self",SC2(91,&h,data),0,0);h.pid=0;
    data[1].effective=~0U;data[1].permitted=~0U;
    capabilities("cred.capset-ignored-high-bits",SC2(91,&h,data));
    data[1].effective=511;data[1].permitted=511;data[0].inheritable=1U<<10;
    capabilities("cred.capset-inheritable",SC2(91,&h,data));
    record("cred.ambient-raise",SC3(167,47,2,10),0,0);
    record("cred.ambient-is-set",SC3(167,47,1,10),0,0);
    record("cred.bounding-drop",SC2(167,24,10),0,0);
    capabilities("cred.bounding-keeps-current",SC2(167,23,10));
    data[0].permitted&=~(1U<<10);data[0].effective=data[0].permitted;
    capabilities("cred.capset-drop-permitted",SC2(91,&h,data));
    record("cred.ambient-intersect",SC3(167,47,1,10),0,0);
    data[0].permitted|=1U<<10;record("cred.capset-no-permitted-gain",SC2(91,&h,data),0,0);
    h.version=0x19980330;data[0]=(struct cred_data){1,1,0};
    capabilities("cred.capset-v1-clears-upper",SC2(91,&h,data));
    record("cred.bounding-permission-before-number",SC2(167,24,99),0,0);
    record("cred.securebits-unpriv-nochange",SC2(167,28,0),0,0);
    record("cred.securebits-userspace-lock",SC2(167,28,0x300),0,0);
    record("cred.securebits-locked",SC2(167,28,0),0,0);
}
static void keepcaps(void)
{
    record("cred.keepcaps-default",SC1(167,7),0,0);
    record("cred.keepcaps-invalid",SC2(167,8,2),0,0);
    record("cred.keepcaps-enable",SC2(167,8,1),0,0);
    capabilities("cred.keepcaps-setuid",SC1(146,1000));
    struct cred_header h={0x20080522,0};struct cred_data data[2]={{0}};abi_require(!SC2(90,&h,data));
    data[0].effective=data[0].permitted;data[1].effective=data[1].permitted;
    capabilities("cred.keepcaps-restore-effective",SC2(91,&h,data));
    state("cred.keepcaps-root-again",SC1(146,0));
    record("cred.keepcaps-lock",SC2(167,28,0x30),0,0);
    record("cred.keepcaps-locked-disable",SC2(167,8,0),0,0);
}
static void secure(void)
{
    record("cred.securebits-invalid",SC2(167,28,0x1000),0,0);
    record("cred.securebits-fixup-disable",SC2(167,28,4),0,0);
    capabilities("cred.securebits-retain-caps",SC1(146,1000));
    record("cred.nnp-invalid",SC2(167,38,0),0,0);
    record("cred.nnp-enable",SC2(167,38,1),0,0);
    record("cred.nnp-query",SC1(167,39),0,0);
    record("cred.nnp-query-extra",SC2(167,39,1),0,0);
    record("cred.nnp-no-clear",SC2(167,38,0),0,0);
    record("cred.dumpable-invalid",SC2(167,4,2),0,0);
    record("cred.dumpable-set",SC2(167,4,1),0,0);
    record("cred.dumpable-query",SC1(167,3),0,0);
    record("cred.ambient-invalid-cap",SC3(167,47,1,41),0,0);
    record("cred.ambient-clear-extra",SC3(167,47,4,1),0,0);
    record("cred.ambient-clear",SC2(167,47,4),0,0);
    record("cred.prctl-unknown",SC1(167,0x7fffffff),0,0);
}
static void isolated(void (*function)(void))
{
    int pipe[2];abi_require(!SC2(59,pipe,0));
    long child=SC5(220,17,0,0,0,0);abi_require(child>=0);
    if(!child){SC1(57,pipe[0]);output=pipe[1];function();abi_exit(0);}
    SC1(57,pipe[1]);
    for(;;){struct credential_packet packet;usize got=0;
        while(got<sizeof(packet)){long r=SC3(63,pipe[0],(char *)&packet+got,sizeof(packet)-got);abi_require(r>=0);if(!r)break;got+=(usize)r;}
        if(!got)break;
        abi_require(got==sizeof(packet));abi_record(packet.name,packet.result,-1,-1,0,packet.data,packet.size);
    }
    SC1(57,pipe[0]);int status=0;abi_require(SC4(260,child,&status,0,0)==child&&!status);
}
void abi_credentials_cases(void)
{isolated(ids);isolated(groups);isolated(caps);isolated(keepcaps);isolated(secure);}
