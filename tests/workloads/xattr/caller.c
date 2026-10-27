#define _GNU_SOURCE
#include <sys/xattr.h>
#include <sys/vfs.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/auxv.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sched.h>
#include <grp.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) {printf("XATTR FAIL line=%d errno=%d\n",__LINE__,errno);fflush(NULL);exit(1);}} while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1 && errno==(e));} while(0)
static unsigned cases;
static void record(const char *name){printf("XATTR CASE %s PASS\n",name);cases++;}
static int has(const char *list,ssize_t size,const char *name)
{for(ssize_t i=0;i<size;i+=(ssize_t)strlen(list+i)+1)if(!strcmp(list+i,name))return 1;return 0;}
static void child_ok(pid_t pid){int status;CHECK(pid>0 && waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));}
static void abi(void)
{
    char out[1024],name[257];int fd=open("file",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
    CHECK(flistxattr(fd,NULL,0)==0);
    CHECK(!setxattr("file","user.a","abc",3,XATTR_CREATE));
    CHECK(getxattr("file","user.a",NULL,0)==3 && fgetxattr(fd,"user.a",out,sizeof(out))==3 && !memcmp(out,"abc",3));
    ERROR(fsetxattr(fd,"user.a","x",1,XATTR_CREATE),EEXIST);
    ERROR(fsetxattr(fd,"user.none","x",1,XATTR_REPLACE),ENODATA);
    ERROR(fsetxattr(fd,"user.a","x",1,3),EEXIST);ERROR(fsetxattr(fd,"user.none","x",1,3),ENODATA);
    CHECK(!fsetxattr(fd,"user.a","abcd",4,XATTR_REPLACE));ERROR(getxattr("file","user.a",out,3),ERANGE);
    CHECK(!fsetxattr(fd,"user.empty",NULL,0,0) && !fgetxattr(fd,"user.empty",NULL,0));
    ssize_t n=listxattr("file",out,sizeof(out));CHECK(n==18 && has(out,n,"user.a") && has(out,n,"user.empty"));
    ERROR(listxattr("file",out,17),ERANGE);
    CHECK(!symlink("file","sym") && getxattr("sym","user.a",NULL,0)==4);
    ERROR(lgetxattr("sym","user.a",out,sizeof(out)),ENODATA);ERROR(lsetxattr("sym","user.a","x",1,0),EPERM);
    CHECK(llistxattr("sym",NULL,0)==0);ERROR(lremovexattr("sym","user.a"),EPERM);
    CHECK(!removexattr("file","user.empty"));ERROR(fremovexattr(fd,"user.empty"),ENODATA);
    int opath=open("file",O_PATH);CHECK(opath>=0);ERROR(fgetxattr(opath,"user.a",NULL,0),EBADF);CHECK(!close(opath));
    memset(name,'n',sizeof(name));memcpy(name,"user.",5);name[255]=0;
    CHECK(!setxattr("file",name,"z",1,0));name[255]='n';name[256]=0;ERROR(getxattr("file",name,NULL,0),ERANGE);
    ERROR(getxattr("file","",NULL,0),ERANGE);ERROR(getxattr("file","user.",NULL,0),EINVAL);
    ERROR(setxattr("file","other.name","z",1,0),EOPNOTSUPP);
    ERROR(syscall(SYS_fsetxattr,-1,"",NULL,0,4),EINVAL);
    ERROR(syscall(SYS_fgetxattr,-1,"",NULL,0),ERANGE);
    ERROR(syscall(SYS_fsetxattr,-1,"user.a",(void *)1,1,0),EFAULT);
    struct statfs type;CHECK(!fstatfs(fd,&type));
    if (type.f_type==0x01021994) {
        int large=open("large-list",O_CREAT|O_RDWR,0600);CHECK(large>=0);
        for (unsigned i=0;i<257;i++) {memset(name,'n',255);memcpy(name,"user.",5);snprintf(name+252,5,"%03u",i);CHECK(!fsetxattr(large,name,"",0,0));}
        CHECK(flistxattr(large,NULL,0)==257*256);
        char *list=malloc(65536);CHECK(list);ERROR(flistxattr(large,list,65536),E2BIG);free(list);CHECK(!close(large) && !unlink("large-list"));
    }
    CHECK(!rename("file","renamed") && !unlink("renamed") && fgetxattr(fd,"user.a",out,sizeof(out))==4);
    CHECK(!fremovexattr(fd,"user.a") && !close(fd) && !unlink("sym"));record("abi-flags-empty-length-follow-unlinked-fd");
}
static void permissions(void)
{
    int fd=open("permissions",O_CREAT|O_RDWR,0644);CHECK(fd>=0);
    CHECK(!fsetxattr(fd,"user.public","p",1,0) && !fsetxattr(fd,"trusted.secret","s",1,0));
    pid_t p=fork();CHECK(p>=0);
    if(!p){char out[64];CHECK(!setgroups(0,NULL) && !setresgid(1000,1000,1000) && !setresuid(1000,1000,1000));
        CHECK(fgetxattr(fd,"user.public",out,1)==1);ERROR(fsetxattr(fd,"user.public","x",1,0),EACCES);
        ERROR(fgetxattr(fd,"trusted.",out,1),ENODATA);ERROR(fsetxattr(fd,"trusted.","x",1,0),EPERM);
        ERROR(fgetxattr(fd,"trusted.secret",out,1),ENODATA);ERROR(fsetxattr(fd,"trusted.secret","x",1,0),EPERM);
        CHECK(flistxattr(fd,out,sizeof(out))==12 && !strcmp(out,"user.public"));_exit(0);}
    child_ok(p);CHECK(!close(fd) && !unlink("permissions"));record("dac-current-identity-trusted-list-filter");
}
static void capability(void)
{
    int fd=open("caps",O_CREAT|O_RDWR,0755);CHECK(fd>=0);uint32_t v2[5]={0x02000001,1<<10,0,0,0},out[6]={0},v3[6]={0x03000001,1<<10,0,0,0,0};
    /* Prime the absence fast path, then replace it while the inode stays open. */
    CHECK(write(fd,"x",1)==1 && !fsetxattr(fd,"security.capability",v2,sizeof(v2),0));
    CHECK(fgetxattr(fd,"security.capability",out,sizeof(out))==20 && !memcmp(out,v2,20));
    CHECK(pwrite(fd,"y",1,0)==1);ERROR(fgetxattr(fd,"security.capability",NULL,0),ENODATA);
    CHECK(!fsetxattr(fd,"security.capability",v3,sizeof(v3),0));
    CHECK(fgetxattr(fd,"security.capability",out,sizeof(out))==20 && !memcmp(out,v2,20));
    v3[5]=1000;CHECK(!fsetxattr(fd,"security.capability",v3,sizeof(v3),0));CHECK(fgetxattr(fd,"security.capability",out,sizeof(out))==24 && out[5]==1000);
    v3[5]=UINT32_MAX;ERROR(fsetxattr(fd,"security.capability",v3,sizeof(v3),0),EINVAL);
    uint32_t bad=0;ERROR(fsetxattr(fd,"security.capability",&bad,4,0),EINVAL);
    CHECK(!fsetxattr(fd,"security.capability",NULL,0,0));ERROR(fgetxattr(fd,"security.capability",NULL,0),EINVAL);
    CHECK(!fremovexattr(fd,"security.capability"));
    CHECK(!fsetxattr(fd,"security.capability",v2,sizeof(v2),0) && !ftruncate(fd,0));ERROR(fgetxattr(fd,"security.capability",NULL,0),ENODATA);
    CHECK(!close(fd) && !unlink("caps"));record("filecap-revision-rootid-empty-killpriv-cache");
}
static void exec_capability(void)
{
    int input=open("/init",O_RDONLY),output=open("exec-cap",O_CREAT|O_EXCL|O_WRONLY,0755);CHECK(input>=0 && output>=0);
    char buffer[4096];ssize_t count;
    while ((count=read(input,buffer,sizeof(buffer)))>0) CHECK(write(output,buffer,(size_t)count)==count);
    CHECK(!count && !close(input) && !close(output));
    uint32_t cap[5]={0x02000001,1<<10,0,0,0};CHECK(!setxattr("exec-cap","security.capability",cap,sizeof(cap),0));
    pid_t child=fork();CHECK(child>=0);
    if (!child) {CHECK(!setgroups(0,NULL) && !setresgid(1000,1000,1000) && !setresuid(1000,1000,1000));execl("exec-cap","exec-cap","--cap-child",(char *)NULL);CHECK(0);}
    child_ok(child);CHECK(!unlink("exec-cap"));record("user-written-filecap-exec-auxv");
}
static void tool(char *const arguments[],const char *expected)
{
    int pipes[2];CHECK(!pipe(pipes));pid_t child=fork();CHECK(child>=0);
    if (!child) {CHECK(!close(pipes[0]) && dup2(pipes[1],1)==1 && !close(pipes[1]));execv(arguments[0],arguments);CHECK(0);}
    CHECK(!close(pipes[1]));char output[512];size_t length=0;ssize_t count;
    while ((count=read(pipes[0],output+length,sizeof(output)-1-length))>0) {length+=(size_t)count;CHECK(length<sizeof(output)-1);}
    CHECK(!count && !close(pipes[0]));output[length]=0;child_ok(child);CHECK(!strcmp(output,expected));
}
static void original_tools(void)
{
    if (access("/xattr-tools/setfattr",X_OK)) return;
    int fd=open("cli",O_CREAT|O_RDWR,0755);CHECK(fd>=0 && !close(fd));
    char *set[]={"/xattr-tools/setfattr","-n","user.cli","-v","value","cli",NULL};tool(set,"");
    char *get[]={"/xattr-tools/getfattr","--only-values","-n","user.cli","cli",NULL};tool(get,"value");
    char *cap[]={"/xattr-tools/setcap","cap_net_bind_service=ep","cli",NULL};tool(cap,"");
    char *show[]={"/xattr-tools/getcap","cli",NULL};tool(show,"cli cap_net_bind_service=ep\n");
    char *clear[]={"/xattr-tools/setcap","-r","cli",NULL};tool(clear,"");tool(show,"");
    char *remove[]={"/xattr-tools/setfattr","-x","user.cli","cli",NULL};tool(remove,"");
    CHECK(!unlink("cli"));record("original-attr-libcap-tools");
}
struct concurrent {int fd;unsigned cpu;pthread_barrier_t gate;};
static void *replace(void *arg)
{
    struct concurrent *c=arg;cpu_set_t mask;CPU_ZERO(&mask);CPU_SET(c->cpu,&mask);
    CHECK(!sched_setaffinity(0,sizeof(mask),&mask));unsigned actual=UINT32_MAX;
    CHECK(!syscall(SYS_getcpu,&actual,NULL,NULL) && actual==c->cpu);
    char value[257];memset(value,0x73,sizeof(value));
    for(unsigned i=0;i<128;i++){int r=pthread_barrier_wait(&c->gate);CHECK(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);CHECK(!fsetxattr(c->fd,"user.race",value,sizeof(value),0));r=pthread_barrier_wait(&c->gate);CHECK(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);}return NULL;
}
static void concurrent(void)
{
    struct concurrent c={.fd=open("concurrent",O_CREAT|O_RDWR,0600)};CHECK(c.fd>=0 && !pthread_barrier_init(&c.gate,NULL,2));
    char initial[257],out[257];memset(initial,0x22,sizeof(initial));CHECK(!fsetxattr(c.fd,"user.race",initial,sizeof(initial),0));pthread_t t;
    CHECK(!unlink("concurrent"));
    unsigned cpus=0;cpu_set_t mask;
    for (unsigned i=0;i<4;i++) {CPU_ZERO(&mask);CPU_SET(i,&mask);if (sched_setaffinity(0,sizeof(mask),&mask)) {CHECK(errno==EINVAL);break;}cpus++;}
    CPU_ZERO(&mask);CPU_SET(0,&mask);CHECK(cpus && !sched_setaffinity(0,sizeof(mask),&mask));
    for (unsigned cpu=0;cpu<cpus;cpu++) {
    c.cpu=cpu;CHECK(!pthread_create(&t,NULL,replace,&c));
    for(unsigned i=0;i<128;i++){int r=pthread_barrier_wait(&c.gate);CHECK(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);CHECK(fgetxattr(c.fd,"user.race",out,sizeof(out))==sizeof(out));for(unsigned j=1;j<sizeof(out);j++)CHECK(out[j]==out[0]);r=pthread_barrier_wait(&c.gate);CHECK(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);}
    CHECK(!pthread_join(t,NULL));
    }
    CHECK(!pthread_barrier_destroy(&c.gate) && !close(c.fd));record("replace-read-unlinked-inode-stable-value");
}
static void fault(void)
{
    long page=sysconf(_SC_PAGESIZE);char *area=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(area!=MAP_FAILED);
    int fd=open("fault",O_CREAT|O_RDWR,0600);CHECK(fd>=0 && !fsetxattr(fd,"user.a","abcdef",6,0));
    CHECK(!mprotect(area+page,page,PROT_NONE));
    ERROR(syscall(SYS_fgetxattr,fd,"user.a",area+page-3,6),EFAULT);CHECK(!memcmp(area+page-3,"abc",3));
    ERROR(syscall(SYS_fsetxattr,fd,"user.a",area+page-3,6,0),EFAULT);char out[6];CHECK(fgetxattr(fd,"user.a",out,6)==6 && !memcmp(out,"abcdef",6));
    CHECK(!munmap(area,page*2) && !close(fd) && !unlink("fault"));record("fault-prefix-input-rollback");
}
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--cap-child")) {
        struct {uint32_t version;int32_t pid;} header={0x20080522,0};
        struct {uint32_t effective,permitted,inheritable;} data[2]={{0}};
        CHECK(!syscall(SYS_capget,&header,data) && getuid()==1000 && geteuid()==1000 &&
            data[0].effective==(1U<<10) && !data[1].effective && getauxval(AT_SECURE)==1);
        return 0;
    }
    ERROR(setxattr("/xattr-immutable","user.fixture","bad",3,0),EPERM);
    ERROR(removexattr("/xattr-immutable","user.fixture"),EPERM);
    ERROR(setxattr("/xattr-append","user.fixture","bad",3,0),EPERM);
    ERROR(removexattr("/xattr-append","user.fixture"),EPERM);
    CHECK(!mkdir("/xattr-check",0755) && !chdir("/xattr-check"));
    abi();permissions();capability();concurrent();fault();exec_capability();original_tools();
    CHECK(!mkdir("memory",0755) && !mount("tmpfs","memory","tmpfs",0,NULL) && !chdir("memory"));
    abi();permissions();capability();concurrent();fault();exec_capability();original_tools();
    CHECK(!chdir("..") && !umount("memory") && !rmdir("memory") && !chdir("/") && !rmdir("/xattr-check"));
    printf("XATTR END %u\n",cases);fflush(NULL);return 0;
}
