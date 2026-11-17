/* Execute the production IPv6 multicast option importer with deterministic
 * user-copy faults. Backend semantics have separate protocol-owner tests. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define KERNEL_EINVAL 22
#define KERNEL_EFAULT 14
#define KERNEL_EOPNOTSUPP 95
#define KERNEL_ENOPROTOOPT 92
#define KERNEL_EPROTO 71
#define KERNEL_SOCKET_AF_INET6 10
#define KERNEL_UACCESS_STATUS_OK 0
struct kernel_mm {int unused;};
struct kernel_socket {int family,type;};
struct kernel_syscall_request {uint64_t arguments[6];};
enum kernel_socket_option {KERNEL_SOCKET_IPV6_MULTICAST_HOPS,KERNEL_SOCKET_IPV6_MULTICAST_LOOP,KERNEL_SOCKET_IPV6_MULTICAST_ALL};
static unsigned reads,writes,calls;static uint64_t write_at[3];
static uintptr_t readable_end;static uint64_t blocked_write;
static uint32_t observed_index;static int observed_value,observed_remove,backend_result;
static unsigned char observed_group[16];static enum kernel_socket_option observed_option;
static int kernel_socket_family(struct kernel_socket *s){return s->family;}
static int kernel_socket_is_unix(struct kernel_socket *s){return s->family==1;}
static int kernel_socket_is_tcp(struct kernel_socket *s){return s->type==1 && (s->family==2 || s->family==10);}
static int kernel_socket_is_stream(struct kernel_socket *s){return s->type==1;}
static int kernel_copy_from_user(struct kernel_mm *m,void *dst,uint64_t src,size_t n,size_t *done)
{(void)m;reads++;*done=0;if(src<4096 || (readable_end && src+n>readable_end))return 1;memcpy(dst,(void *)(uintptr_t)src,n);*done=n;return 0;}
static int kernel_copy_to_user(struct kernel_mm *m,uint64_t dst,const void *src,size_t n,size_t *done)
{(void)m;CHECK(writes<3);write_at[writes++]=dst;*done=0;if(dst<4096 || dst==blocked_write)return 1;memcpy((void *)(uintptr_t)dst,src,n);*done=n;return 0;}
static int kernel_socket_get_ipv6_multicast_interface(struct kernel_socket *s,uint32_t *v)
{(void)s;calls++;*v=0x12345678;return backend_result;}
static int kernel_socket_set_ipv6_multicast_interface(struct kernel_socket *s,uint32_t v)
{(void)s;calls++;observed_index=v;return backend_result;}
static int kernel_socket_get_option(struct kernel_socket *s,enum kernel_socket_option opt,int *v)
{(void)s;calls++;observed_option=opt;*v=0x12345678;return backend_result;}
static int kernel_socket_set_option(struct kernel_socket *s,enum kernel_socket_option opt,int v)
{(void)s;calls++;observed_option=opt;observed_value=v;return backend_result;}
static int kernel_socket_ipv6_membership(struct kernel_socket *s,const uint8_t group[16],uint32_t index,int remove)
{(void)s;calls++;memcpy(observed_group,group,16);observed_index=index;observed_remove=remove;return backend_result;}
#include "ipv6_multicast_sockopt.h"
static int run(struct kernel_socket *s,int opt,void *value,int32_t *length,int get)
{struct kernel_syscall_request r={.arguments={0,41,(unsigned)opt,(uintptr_t)value,get?(uintptr_t)length:(uint64_t)(int64_t)*length}};
 reads=writes=calls=0;return ipv6_multicast_sockopt(s,NULL,&r,get);}
int main(void)
{
 struct kernel_socket d={10,2},t={10,1},v4={2,2};int32_t n;int value;
 for(int opt=17;opt<=29;opt++)if(opt<=21 || opt==29){
  int member=opt==20 || opt==21;
  n=3;value=7;CHECK(run(&d,opt,&value,&n,0)==-22 && !reads && !calls);
  n=4;CHECK(run(&d,opt,(void *)1,&n,0)==-14 && reads==1 && !calls);
  n=4;CHECK(run(&d,opt,NULL,&n,0)==(member?-22:0) && !reads && calls==!member);
  if(!member)CHECK((opt==17?observed_index:(uint32_t)observed_value)==0);
  n=-1;CHECK(run(&d,opt,&value,&n,0)==-22 && !reads && !calls);
  n=-1;value=0;CHECK(run(&d,opt,&value,&n,1)==(member?-92:0));
  if(!member)CHECK(n==4 && value==0x12345678 && writes==2);
  n=0;CHECK(run(&d,opt,NULL,&n,1)==(member?-92:0));if(!member)CHECK(writes==1 && n==0);
  n=4;CHECK(run(&v4,opt,(void *)1,&n,0)==-92 && !reads);
  n=4;CHECK(run(&v4,opt,(void *)1,&n,1)==-95 && !reads);
 }
 for(int opt=17;opt<=18;opt++){
  n=0;CHECK(run(&t,opt,NULL,&n,0)==-92 && !reads && !calls);
  n=4;CHECK(run(&t,opt,(void *)1,&n,0)==-14);
  n=4;value=0;CHECK(!run(&t,opt,&value,&n,1) && value==0x12345678);
 }
 unsigned char mreq[20]={0xff,2,0,0};uint32_t index=257;memcpy(mreq+16,&index,4);
 for(int opt=20;opt<=21;opt++){
  n=19;CHECK(run(&d,opt,mreq,&n,0)==-22 && reads==1 && !calls);
  n=20;CHECK(run(&t,opt,NULL,&n,0)==-71 && !reads && !calls);
  n=20;CHECK(run(&d,opt,NULL,&n,0)==-14 && reads==1);
  n=20;CHECK(run(&t,opt,(void *)1,&n,0)==-14 && reads==1);
  readable_end=(uintptr_t)mreq+4;n=20;CHECK(run(&d,opt,mreq,&n,0)==-14 && reads==2 && !calls);readable_end=0;
  n=100;CHECK(!run(&d,opt,mreq,&n,0) && reads==2 && calls==1 && observed_index==257 && observed_remove==(opt==21) && !memcmp(mreq,observed_group,16));
 }
 for(n=1;n<=4;n++){
  int32_t length=n;value=0;CHECK(!run(&d,17,&value,&length,1));
  CHECK(length==n && write_at[0]==(uintptr_t)&length && write_at[1]==(uintptr_t)&value);
  CHECK(!memcmp(&value,(uint32_t[]){0x12345678},(size_t)n));
 }
 n=1;value=0;blocked_write=(uintptr_t)&value;CHECK(run(&d,18,&value,&n,1)==-14 && n==1 && writes==2);blocked_write=0;
 n=4;blocked_write=(uintptr_t)&n;CHECK(run(&d,18,&value,&n,1)==-14 && writes==1);blocked_write=0;
 backend_result=-19;n=4;CHECK(run(&d,17,&value,&n,1)==-19 && !writes);backend_result=0;
 puts("PASS IPv6 multicast sockopt import: native int/mreq, fault precedence, NULL, short/negative get, family/type and output ordering");
}
