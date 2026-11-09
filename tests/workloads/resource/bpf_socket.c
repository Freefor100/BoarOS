#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/bpf.h>
#include <sys/syscall.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <stddef.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"BPF SOCKET FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x);exit(1);}}while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1 && errno==(e));}while(0)
static void attach(int fd,struct sock_filter *code,unsigned count)
{struct sock_fprog p={.len=count,.filter=code};CHECK(!setsockopt(fd,SOL_SOCKET,SO_ATTACH_FILTER,&p,sizeof(p)));}
static void constant(int fd,unsigned value)
{struct sock_filter code[]={BPF_STMT(BPF_RET|BPF_K,value)};attach(fd,code,1);}
static void detach(int fd)
{int zero=0;CHECK(!setsockopt(fd,SOL_SOCKET,SO_DETACH_FILTER,&zero,sizeof(zero)));}
static void absent(int fd)
{char data[32];ERROR(recv(fd,data,sizeof(data),MSG_DONTWAIT),EAGAIN);}
static int bound(int family,int type,struct sockaddr_storage *address,socklen_t *length)
{
 int fd=socket(family,type,0);CHECK(fd>=0);memset(address,0,sizeof(*address));
 if(family==AF_INET){struct sockaddr_in *a=(void *)address;a->sin_family=family;a->sin_addr.s_addr=htonl(INADDR_LOOPBACK);*length=sizeof(*a);}
 else {struct sockaddr_in6 *a=(void *)address;a->sin6_family=family;a->sin6_addr=in6addr_loopback;*length=sizeof(*a);}
 CHECK(!bind(fd,(void *)address,*length));CHECK(!getsockname(fd,(void *)address,length));return fd;
}
static void send_data(int fd,const struct sockaddr_storage *a,socklen_t n,const char *data,unsigned size)
{CHECK(sendto(fd,data,size,0,(const void *)a,n)==size);}
static void udp(int family)
{
 struct sockaddr_storage a;socklen_t n;int rx=bound(family,SOCK_DGRAM,&a,&n),tx=socket(family,SOCK_DGRAM,0);CHECK(tx>=0);
 struct sock_filter select[]={BPF_STMT(BPF_LD|BPF_B|BPF_ABS,8),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,'Y',0,1),BPF_STMT(BPF_RET|BPF_K,11),BPF_STMT(BPF_RET|BPF_K,0)};
 attach(rx,select,4);send_data(tx,&a,n,"Nope",4);absent(rx);send_data(tx,&a,n,"Yes!!!",6);
 char out[32]={0};CHECK(recv(rx,out,sizeof(out),0)==3 && !memcmp(out,"Yes",3));
 struct sock_filter original[4];socklen_t count=0;CHECK(!getsockopt(rx,SOL_SOCKET,SO_GET_FILTER,NULL,&count) && count==4);
 count=3;ERROR(getsockopt(rx,SOL_SOCKET,SO_GET_FILTER,original,&count),EINVAL);
 count=4;CHECK(!getsockopt(rx,SOL_SOCKET,SO_GET_FILTER,original,&count) && count==4 && !memcmp(select,original,sizeof(select)));
 struct sock_filter length[]={BPF_STMT(BPF_LD|BPF_W|BPF_LEN,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,12,0,1),BPF_STMT(BPF_RET|BPF_K,UINT32_MAX),BPF_STMT(BPF_RET|BPF_K,0)};
 attach(rx,length,4);send_data(tx,&a,n,"123",3);absent(rx);send_data(tx,&a,n,"1234",4);CHECK(recv(rx,out,sizeof(out),0)==4);
 struct sock_filter oob[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,65535),BPF_STMT(BPF_RET|BPF_K,UINT32_MAX)};
 attach(rx,oob,2);send_data(tx,&a,n,"1234",4);absent(rx);
 struct sock_filter network[]={BPF_STMT(BPF_LD|BPF_B|BPF_ABS,(unsigned)SKF_NET_OFF),BPF_STMT(BPF_ALU|BPF_AND|BPF_K,0xf0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,family==AF_INET?0x40:0x60,0,1),BPF_STMT(BPF_RET|BPF_K,UINT32_MAX),BPF_STMT(BPF_RET|BPF_K,0)};
 attach(rx,network,5);send_data(tx,&a,n,"ip",2);CHECK(recv(rx,out,sizeof(out),0)==2);
 constant(rx,1);send_data(tx,&a,n,"1234",4);CHECK(recv(rx,out,sizeof(out),0)==0);
 int alias=dup(rx);CHECK(alias>=0);detach(alias);send_data(tx,&a,n,"1234",4);CHECK(recv(rx,out,sizeof(out),0)==4);
 int ready[2],go[2];CHECK(!pipe(ready) && !pipe(go));pid_t child=fork();CHECK(child>=0);
 if(!child){close(ready[0]);close(go[1]);for(unsigned i=0;i<32;i++){constant(alias,0);CHECK(write(ready[1],"x",1)==1);CHECK(read(go[0],out,1)==1);constant(alias,UINT32_MAX);CHECK(write(ready[1],"x",1)==1);CHECK(read(go[0],out,1)==1);}_exit(0);}
 close(ready[1]);close(go[0]);for(unsigned i=0;i<32;i++){CHECK(read(ready[0],out,1)==1);send_data(tx,&a,n,"drop",4);absent(rx);CHECK(write(go[1],"x",1)==1);CHECK(read(ready[0],out,1)==1);send_data(tx,&a,n,"pass",4);CHECK(recv(rx,out,sizeof(out),0)==4);CHECK(write(go[1],"x",1)==1);}
 int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));close(ready[0]);close(go[1]);
 int one=1,zero=0;CHECK(!setsockopt(rx,SOL_SOCKET,SO_LOCK_FILTER,&one,4));ERROR(setsockopt(rx,SOL_SOCKET,SO_LOCK_FILTER,&zero,4),EPERM);ERROR(setsockopt(alias,SOL_SOCKET,SO_DETACH_FILTER,&zero,4),EPERM);
 struct sock_filter ret[]={BPF_STMT(BPF_RET|BPF_K,0)};struct sock_fprog f={1,ret};ERROR(setsockopt(alias,SOL_SOCKET,SO_ATTACH_FILTER,&f,sizeof(f)),EPERM);
 CHECK(!close(alias) && !close(rx) && !close(tx));printf("BPF SOCKET CASE udp%d PASS\n",family);
}
static void unix_packets(void)
{
 for(int type=SOCK_STREAM;type<=SOCK_SEQPACKET;type++){if(type!=SOCK_STREAM && type!=SOCK_DGRAM && type!=SOCK_SEQPACKET)continue;
 int pair[2];CHECK(!socketpair(AF_UNIX,type,0,pair));constant(pair[1],0);CHECK(send(pair[0],"test",4,0)==4);char data[8];
 if(type==SOCK_STREAM)CHECK(recv(pair[1],data,8,0)==4);else absent(pair[1]);
 if(type!=SOCK_STREAM) {
  int pipefd[2];CHECK(!pipe(pipefd));char control[CMSG_SPACE(sizeof(int))]={0};struct iovec iov={.iov_base=(void *)"right",.iov_len=5};
  struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&pipefd[1],sizeof(int));
  CHECK(sendmsg(pair[0],&msg,0)==5);CHECK(!close(pipefd[1]));CHECK(!fcntl(pipefd[0],F_SETFL,O_NONBLOCK));CHECK(read(pipefd[0],data,1)==0);close(pipefd[0]);absent(pair[1]);
 }
 constant(pair[1],2);CHECK(send(pair[0],"test",4,0)==4);CHECK(recv(pair[1],data,8,0)==(type==SOCK_STREAM?4:2));close(pair[0]);close(pair[1]);}
 puts("BPF SOCKET CASE unix PASS");
}
static void unix_full_queue(void)
{
 int rx=socket(AF_UNIX,SOCK_DGRAM,0),tx=socket(AF_UNIX,SOCK_DGRAM,0);CHECK(rx>=0 && tx>=0);
 struct sockaddr_un address={.sun_family=AF_UNIX};int used=snprintf(address.sun_path+1,sizeof(address.sun_path)-1,"bpf-filter-%ld",(long)getpid());socklen_t length=offsetof(struct sockaddr_un,sun_path)+1+used;
 CHECK(!bind(rx,(void *)&address,length));
 /* Independent senders isolate receiver queue admission from sender memory. */
 unsigned count=0;while(sendto(tx,"queued",6,MSG_DONTWAIT,(void *)&address,length)==6){CHECK(++count<4096);CHECK(!close(tx));tx=socket(AF_UNIX,SOCK_DGRAM,0);CHECK(tx>=0);}CHECK(errno==EAGAIN && count>0);
 constant(rx,0);CHECK(sendto(tx,"dropped",7,MSG_DONTWAIT,(void *)&address,length)==7);
 char data[8];for(unsigned i=0;i<count;i++)CHECK(recv(rx,data,sizeof(data),0)==6);absent(rx);
 close(tx);close(rx);puts("BPF SOCKET CASE unix-full-queue-drop PASS");
}
static void tcp(void)
{
 struct sockaddr_storage a;socklen_t n;int listener=bound(AF_INET,SOCK_STREAM,&a,&n);CHECK(!listen(listener,4));constant(listener,0);
 int client=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);CHECK(client>=0);ERROR(connect(client,(void *)&a,n),EINPROGRESS);
 struct pollfd p={client,POLLOUT,0};CHECK(poll(&p,1,50)==0);
 constant(listener,UINT32_MAX);int one=1;CHECK(!setsockopt(listener,SOL_SOCKET,SO_LOCK_FILTER,&one,4));CHECK(poll(&p,1,5000)==1);
 int accepted=accept(listener,NULL,NULL);CHECK(accepted>=0);int zero=0;ERROR(setsockopt(accepted,SOL_SOCKET,SO_DETACH_FILTER,&zero,4),EPERM);
 struct sock_filter code; socklen_t count=1;CHECK(!getsockopt(accepted,SOL_SOCKET,SO_GET_FILTER,&code,&count) && count==1 && code.k==UINT32_MAX);
 CHECK(send(client,"tcp",3,0)==3);char out[8];CHECK(recv(accepted,out,8,0)==3 && !memcmp(out,"tcp",3));close(accepted);close(client);close(listener);puts("BPF SOCKET CASE tcp PASS");
}
static void abi(void)
{
 int fd=socket(AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);int zero=0;
 ERROR(setsockopt(fd,SOL_SOCKET,SO_DETACH_FILTER,NULL,0),EINVAL);ERROR(setsockopt(fd,SOL_SOCKET,SO_DETACH_FILTER,NULL,4),EFAULT);ERROR(setsockopt(fd,SOL_SOCKET,SO_DETACH_FILTER,&zero,4),ENOENT);
 struct sock_fprog f={0,NULL};ERROR(setsockopt(fd,SOL_SOCKET,SO_ATTACH_FILTER,&f,sizeof(f)),EINVAL);ERROR(setsockopt(fd,SOL_SOCKET,SO_ATTACH_FILTER,&f,4),EINVAL);
 f.len=1;ERROR(setsockopt(fd,SOL_SOCKET,SO_ATTACH_FILTER,&f,sizeof(f)),EINVAL);f.filter=(void *)1;ERROR(setsockopt(fd,SOL_SOCKET,SO_ATTACH_FILTER,&f,sizeof(f)),EFAULT);
 close(fd);puts("BPF SOCKET CASE abi PASS");
}
static void ebpf(void)
{
 struct bpf_insn code[]={{.code=0xb7,.imm=-1},{.code=0x95}};
 union bpf_attr attr={.prog_type=BPF_PROG_TYPE_SOCKET_FILTER,.insn_cnt=2,.insns=(uintptr_t)code,.license=(uintptr_t)"GPL"};
 int program=syscall(SYS_bpf,BPF_PROG_LOAD,&attr,sizeof(attr));CHECK(program>=0);
 struct sockaddr_storage address;socklen_t length;int rx=bound(AF_INET,SOCK_DGRAM,&address,&length),tx=socket(AF_INET,SOCK_DGRAM,0);CHECK(tx>=0);
 CHECK(!setsockopt(rx,SOL_SOCKET,SO_ATTACH_BPF,&program,4));CHECK(!close(program));
 socklen_t count=0;ERROR(getsockopt(rx,SOL_SOCKET,SO_GET_FILTER,NULL,&count),EACCES);
 send_data(tx,&address,length,"alive",5);char data[8];CHECK(recv(rx,data,8,0)==5 && !memcmp(data,"alive",5));detach(rx);close(rx);close(tx);puts("BPF SOCKET CASE ebpf-fd-lifetime PASS");
}
int main(int argc,char **argv)
{alarm(30);abi();udp(AF_INET);udp(AF_INET6);unix_packets();unix_full_queue();if(argc==2 && !strcmp(argv[1],"unprivileged")){puts("BPF SOCKET unprivileged subset PASS");return 0;}tcp();ebpf();puts("BPF SOCKET PASS");return 0;}
