#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"UTS FAIL %d %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
#define NS_GET_NSTYPE 0xb703U
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#define ALL_NAMESPACES (CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWIPC)
union sem_value { int val;struct semid_ds *buf;unsigned short *array; };
struct namespace_ids { uint64_t value[3]; };
static const char *namespace_names[]={"mnt","uts","ipc"};
static struct namespace_ids current_ids(void)
{
    struct namespace_ids result;
    for(unsigned i=0;i<3;i++) {
        char path[80];snprintf(path,sizeof(path),"/proc/thread-self/ns/%s",namespace_names[i]);
        int fd=open(path,O_RDONLY);struct stat stat;CHECK(fd>=0&&!fstat(fd,&stat)&&!close(fd));
        result.value[i]=stat.st_ino;
    }
    return result;
}
static int same_ids(struct namespace_ids a,struct namespace_ids b)
{ return !memcmp(&a,&b,sizeof(a)); }
static int semaphore(key_t key,int value)
{
    int id=semget(key,1,IPC_CREAT|IPC_EXCL|0600);CHECK(id>=0);
    CHECK(!semctl(id,0,SETVAL,(union sem_value){.val=value}));return id;
}

static void names(const char *host,const char *domain)
{
    struct utsname name;CHECK(!uname(&name));
    CHECK(!strcmp(name.nodename,host)&&!strcmp(name.domainname,domain));
}
static void set_names(const char *host,const char *domain)
{
    CHECK(!syscall(SYS_sethostname,host,strlen(host)));
    CHECK(!syscall(SYS_setdomainname,domain,strlen(domain)));
}
static void wait_ok(pid_t child)
{
    int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
}
static void byte_write(int fd)
{ CHECK(write(fd,"x",1)==1); }
static void byte_read(int fd)
{ char byte;CHECK(read(fd,&byte,1)==1&&byte=='x'); }
static void proc_host(int fd,const char *host)
{
    char text[80];CHECK(lseek(fd,0,SEEK_SET)==0);
    ssize_t length=read(fd,text,sizeof(text));
    CHECK(length==(ssize_t)strlen(host)+1&&!memcmp(text,host,strlen(host))&&text[length-1]=='\n');
}
static void fd_contract(void)
{
    const char *paths[]={"/proc/self/ns/mnt","/proc/self/ns/uts","/proc/self/ns/ipc"};
    const int types[]={CLONE_NEWNS,CLONE_NEWUTS,CLONE_NEWIPC};
    uint64_t ids[3];
    for(unsigned i=0;i<3;i++) {
        int fd=open(paths[i],O_RDONLY);CHECK(fd>=0&&ioctl(fd,NS_GET_NSTYPE)==types[i]);
        struct stat stat;struct statfs fs;
        CHECK(!fstat(fd,&stat)&&S_ISREG(stat.st_mode)&&stat.st_nlink==1);
        CHECK(!fstatfs(fd,&fs)&&fs.f_type==0x6e736673);ids[i]=stat.st_ino;
        char text[80];ssize_t n=readlink(paths[i],text,sizeof(text)-1);CHECK(n>0);text[n]=0;
        char wanted[80];snprintf(wanted,sizeof(wanted),"%s:[%llu]",i==0?"mnt":i==1?"uts":"ipc",(unsigned long long)ids[i]);
        CHECK(!strcmp(text,wanted));
        struct pollfd pollfd={.fd=fd,.events=POLLIN|POLLOUT};
        CHECK(poll(&pollfd,1,0)==1&&(pollfd.revents&(POLLIN|POLLOUT))==(POLLIN|POLLOUT));
        CHECK(read(fd,text,1)==-1&&errno==EINVAL);
        CHECK(ioctl(fd,0xb7ffU)==-1&&errno==ENOTTY);
        int alias=dup(fd);CHECK(alias>=0&&!close(fd)&&ioctl(alias,NS_GET_NSTYPE)==types[i]&&!close(alias));
        fd=open(paths[i],O_PATH);CHECK(fd>=0);
        CHECK(ioctl(fd,NS_GET_NSTYPE)==-1&&errno==EBADF);CHECK(!close(fd));
        CHECK(open(paths[i],O_WRONLY)==-1&&errno==EPERM);
        CHECK(open(paths[i],O_RDWR)==-1&&errno==EPERM);
        CHECK(open(paths[i],O_RDONLY|O_TRUNC)==-1&&errno==EPERM);
        CHECK(open(paths[i],O_RDONLY|O_DIRECTORY)==-1&&errno==ENOTDIR);
    }
    CHECK(ids[0]!=ids[1]&&ids[0]!=ids[2]&&ids[1]!=ids[2]);
    puts("UTS CASE namespace-fd PASS");
}
static void unshare_and_join(int original)
{
    struct utsname saved;CHECK(!uname(&saved));
    int control=open("/proc/sys/kernel/hostname",O_RDWR);CHECK(control>=0);
    int ready[2],resume[2];CHECK(!pipe(ready)&&!pipe(resume));
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!unshare(CLONE_NEWUTS));names(saved.nodename,saved.domainname);
        set_names("uts-child","child.domain");
        /* This FD was opened before unshare: every operation selects current UTS. */
        proc_host(control,"uts-child");CHECK(lseek(control,0,SEEK_SET)==0&&write(control,"fd-child\n",9)==9);
        names("fd-child","child.domain");byte_write(ready[1]);byte_read(resume[0]);_exit(0);
    }
    byte_read(ready[0]);names(saved.nodename,saved.domainname);proc_host(control,saved.nodename);
    char path[80];snprintf(path,sizeof(path),"/proc/%d/ns/uts",child);
    int target=open(path,O_RDONLY);CHECK(target>=0);
    int pidfd=syscall(SYS_pidfd_open,child,0);CHECK(pidfd>=0);
    CHECK(!setns(pidfd,CLONE_NEWUTS));names("fd-child","child.domain");
    CHECK(!setns(original,0));names(saved.nodename,saved.domainname);
    byte_write(resume[1]);wait_ok(child);CHECK(!close(pidfd));
    /* The namespace FD, rather than the source task, owns its remaining life. */
    CHECK(!setns(target,CLONE_NEWUTS));names("fd-child","child.domain");proc_host(control,"fd-child");
    CHECK(setns(target,CLONE_NEWIPC)==-1&&errno==EINVAL);names("fd-child","child.domain");
    CHECK(setns(target,CLONE_NEWUTS|CLONE_NEWIPC)==-1&&errno==EINVAL);
    CHECK(!setns(original,CLONE_NEWUTS));names(saved.nodename,saved.domainname);
    CHECK(!close(target)&&!close(control));
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(resume[0])&&!close(resume[1]));
    puts("UTS CASE unshare-pidfd-fd-lifetime PASS");
}
static int cloned(void *argument)
{
    (void)argument;names("copy-source","copy.domain");
    set_names("clone-private","clone.domain");return 0;
}
static void clone_fork_exec(const char *executable,int original)
{
    CHECK(!unshare(CLONE_NEWUTS));set_names("copy-source","copy.domain");
    void *stack=malloc(65536);CHECK(stack);
    pid_t child=clone(cloned,(char *)stack+65536,CLONE_NEWUTS|SIGCHLD,NULL);CHECK(child>=0);
    wait_ok(child);free(stack);names("copy-source","copy.domain");
    child=fork();CHECK(child>=0);
    if(!child){set_names("fork-shared","fork.domain");_exit(0);}
    wait_ok(child);names("fork-shared","fork.domain");
    child=fork();CHECK(child>=0);
    if(!child){execl(executable,executable,"--exec","fork-shared","fork.domain",NULL);CHECK(0);}
    wait_ok(child);names("fork-shared","fork.domain");
    CHECK(!setns(original,0));puts("UTS CASE clone-fork-exec PASS");
}
static void *thread_private(void *unused)
{
    (void)unused;CHECK(!unshare(CLONE_NEWUTS));set_names("thread-private","thread.domain");
    names("thread-private","thread.domain");return NULL;
}
static void thread_and_permissions(int original)
{
    struct utsname saved;CHECK(!uname(&saved));
    pthread_t thread;CHECK(!pthread_create(&thread,NULL,thread_private,NULL)&&!pthread_join(thread,NULL));
    names(saved.nodename,saved.domainname);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!setuid(123));
        CHECK(unshare(CLONE_NEWUTS)==-1&&errno==EPERM);
        CHECK(setns(original,0)==-1&&errno==EPERM);
        CHECK(syscall(SYS_sethostname,"denied",6)==-1&&errno==EPERM);
        _exit(0);
    }
    wait_ok(child);names(saved.nodename,saved.domainname);
    puts("UTS CASE thread-and-permissions PASS");
}

struct clone_arguments {
    uint64_t flags,pidfd,child_tid,parent_tid,exit_signal,stack,stack_size,tls,
        set_tid,set_tid_size,cgroup;
};
struct namespace_target { pid_t pid;int release;struct namespace_ids ids; };
static struct namespace_target spawn_target(key_t key,const char *host,int value)
{
    int ready[2],release[2];CHECK(!pipe(ready)&&!pipe(release));
    struct utsname before;CHECK(!uname(&before));
    struct clone_arguments args={.flags=ALL_NAMESPACES,.exit_signal=SIGCHLD};
    pid_t child=(pid_t)syscall(435,&args,sizeof(args));CHECK(child>=0);
    if(!child) {
        names(before.nodename,before.domainname);
        CHECK(semget(key,1,0)==-1&&errno==ENOENT);
        set_names(host,"combined.domain");int sem=semaphore(key,value);
        struct namespace_ids ids=current_ids();
        CHECK(write(ready[1],&ids,sizeof(ids))==sizeof(ids));byte_read(release[0]);
        CHECK(!semctl(sem,0,IPC_RMID));_exit(0);
    }
    struct namespace_target target={.pid=child,.release=release[1]};
    CHECK(read(ready[0],&target.ids,sizeof(target.ids))==sizeof(target.ids));
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(release[0]));return target;
}
struct shared_fs_case { int pidfd,sem,ready,release;key_t key;struct namespace_ids target; };
static int shared_fs_changes(void *argument)
{
    struct shared_fs_case *a=argument;struct namespace_ids original=current_ids();
    struct utsname before;CHECK(!uname(&before));
    struct sembuf undo={0,-1,SEM_UNDO};CHECK(!semop(a->sem,&undo,1));
    CHECK(setns(a->pidfd,CLONE_NEWNS)==-1&&errno==EINVAL);
    CHECK(same_ids(original,current_ids())&&semctl(a->sem,0,GETVAL)==4);
    names(before.nodename,before.domainname);byte_write(a->ready);byte_read(a->release);
    /* Combined pidfd setns prepares a temporary FS, then changes the shared
     * paths while selecting namespaces for this caller alone. */
    CHECK(!setns(a->pidfd,ALL_NAMESPACES)&&same_ids(a->target,current_ids()));
    names("combined-a","combined.domain");int selected=semget(a->key,1,0);
    CHECK(selected>=0&&semctl(selected,0,GETVAL)==11);
    byte_write(a->ready);byte_read(a->release);
    /* A subsequent unshare copies the shared FS before publishing its new
     * namespaces. A cwd change in that independent record stays private. */
    CHECK(!unshare(ALL_NAMESPACES));struct namespace_ids after=current_ids();
    for(unsigned i=0;i<3;i++)CHECK(after.value[i]!=a->target.value[i]);
    CHECK(semget(a->key,1,0)==-1&&errno==ENOENT);
    CHECK(!chdir("/proc"));
    byte_write(a->ready);byte_read(a->release);return 0;
}
static void combined_body(void)
{
    const key_t key=(key_t)(0x4b000000U+(unsigned)getpid());
    struct namespace_ids original=current_ids();int sem=semaphore(key,5);
    struct namespace_target a=spawn_target(key,"combined-a",11),b=spawn_target(key,"combined-b",22);
    for(unsigned i=0;i<3;i++)CHECK(a.ids.value[i]!=original.value[i]&&b.ids.value[i]!=original.value[i]&&a.ids.value[i]!=b.ids.value[i]);
    int first=(int)syscall(SYS_pidfd_open,a.pid,0),second=(int)syscall(SYS_pidfd_open,b.pid,0);
    CHECK(first>=0&&second>=0);
    int ready[2],release[2];CHECK(!pipe(ready)&&!pipe(release));
    CHECK(!chdir("/proc"));
    struct shared_fs_case failure={first,sem,ready[1],release[0],key,a.ids};
    void *stack=malloc(65536);CHECK(stack);
    pid_t child=clone(shared_fs_changes,(char *)stack+65536,CLONE_FS|SIGCHLD,&failure);CHECK(child>=0);
    char cwd[80];
    byte_read(ready[0]);CHECK(semctl(sem,0,GETVAL)==4&&same_ids(original,current_ids()));
    CHECK(getcwd(cwd,sizeof(cwd))&&!strcmp(cwd,"/proc"));byte_write(release[1]);
    byte_read(ready[0]);CHECK(semctl(sem,0,GETVAL)==5&&same_ids(original,current_ids()));
    CHECK(getcwd(cwd,sizeof(cwd))&&!strcmp(cwd,"/"));byte_write(release[1]);
    byte_read(ready[0]);CHECK(semctl(sem,0,GETVAL)==5&&same_ids(original,current_ids()));
    CHECK(getcwd(cwd,sizeof(cwd))&&!strcmp(cwd,"/"));byte_write(release[1]);
    wait_ok(child);free(stack);CHECK(!semctl(sem,0,IPC_RMID));
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(release[0])&&!close(release[1]));
    /* Each round releases a target from A to B while the reader requests all
     * three namespaces. A mixed FS/UTS/IPC tuple is never a valid snapshot. */
    int staged[2],go[2],done[2],finish[2];
    CHECK(!pipe(staged)&&!pipe(go)&&!pipe(done)&&!pipe(finish));
    child=fork();CHECK(child>=0);
    if(!child) {
        for(unsigned i=0;i<8;i++) {
            CHECK(!setns(first,ALL_NAMESPACES));byte_write(staged[1]);byte_read(go[0]);
            CHECK(!setns(second,ALL_NAMESPACES));byte_write(done[1]);
        }
        byte_read(finish[0]);_exit(0);
    }
    int moving=(int)syscall(SYS_pidfd_open,child,0);CHECK(moving>=0);
    for(unsigned i=0;i<8;i++) {
        byte_read(staged[0]);byte_write(go[1]);CHECK(!setns(moving,ALL_NAMESPACES));
        struct namespace_ids observed=current_ids();int which=same_ids(observed,a.ids)?1:same_ids(observed,b.ids)?2:0;
        CHECK(which);names(which==1?"combined-a":"combined-b","combined.domain");
        int selected=semget(key,1,0);CHECK(selected>=0&&semctl(selected,0,GETVAL)==(which==1?11:22));
        byte_read(done[0]);
    }
    byte_write(finish[1]);wait_ok(child);CHECK(!close(moving));
    CHECK(!close(staged[0])&&!close(staged[1])&&!close(go[0])&&!close(go[1]));
    CHECK(!close(done[0])&&!close(done[1])&&!close(finish[0])&&!close(finish[1]));
    byte_write(a.release);byte_write(b.release);wait_ok(a.pid);wait_ok(b.pid);
    CHECK(!close(a.release)&&!close(b.release)&&!close(first)&&!close(second));
}
static void combined_clone3_pidfd(void)
{
    struct namespace_ids before=current_ids();pid_t child=fork();CHECK(child>=0);
    if(!child){combined_body();_exit(0);}wait_ok(child);CHECK(same_ids(before,current_ids()));
    puts("UTS CASE clone3-pidfd-combined-atomic-failure-undo PASS");
}
struct ipc_thread_case { key_t key;int original,ready,release; };
static void *ipc_thread(void *argument)
{
    struct ipc_thread_case *a=argument;CHECK(!unshare(CLONE_NEWIPC));
    CHECK(semget(a->key,1,0)==-1&&errno==ENOENT);int own=semaphore(a->key,27);
    byte_write(a->ready);byte_read(a->release);CHECK(semctl(own,0,GETVAL)==27);
    CHECK(!setns(a->original,CLONE_NEWIPC));int parent=semget(a->key,1,0);
    CHECK(parent>=0&&semctl(parent,0,GETVAL)==9);return NULL;
}
static void same_group_ipc(void)
{
    key_t key=(key_t)(0x4c000000U+(unsigned)getpid());int sem=semaphore(key,9);
    struct namespace_ids before=current_ids();int ready[2],release[2];CHECK(!pipe(ready)&&!pipe(release));
    int original=open("/proc/thread-self/ns/ipc",O_RDONLY);CHECK(original>=0);
    struct ipc_thread_case args={key,original,ready[1],release[0]};pthread_t thread;
    CHECK(!pthread_create(&thread,NULL,ipc_thread,&args));byte_read(ready[0]);
    CHECK(same_ids(before,current_ids())&&semctl(sem,0,GETVAL)==9);byte_write(release[1]);
    CHECK(!pthread_join(thread,NULL)&&!semctl(sem,0,IPC_RMID)&&!close(original));
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(release[0])&&!close(release[1]));
    puts("UTS CASE same-group-independent-ipc PASS");
}
static void *namespace_exec_thread(void *executable)
{
    struct namespace_ids before=current_ids();CHECK(!unshare(CLONE_NEWUTS|CLONE_NEWIPC));
    set_names("nonleader-private","nonleader.domain");key_t key=(key_t)(0x4d000000U+(unsigned)getpid());
    (void)semaphore(key,39);struct namespace_ids after=current_ids();
    CHECK(after.value[1]!=before.value[1]&&after.value[2]!=before.value[2]);
    char key_text[24],uts[24],ipc[24];snprintf(key_text,sizeof(key_text),"%u",(unsigned)key);
    snprintf(uts,sizeof(uts),"%llu",(unsigned long long)after.value[1]);
    snprintf(ipc,sizeof(ipc),"%llu",(unsigned long long)after.value[2]);
    execl(executable,executable,"--namespace-exec",key_text,uts,ipc,NULL);CHECK(0);return NULL;
}
static void nonleader_namespace_exec(const char *executable)
{
    struct namespace_ids before=current_ids();pid_t child=fork();CHECK(child>=0);
    if(!child) {
        pthread_t thread;CHECK(!pthread_create(&thread,NULL,namespace_exec_thread,(void *)executable));
        for(;;)pause();
    }
    wait_ok(child);CHECK(same_ids(before,current_ids()));
    puts("UTS CASE nonleader-exec-current-namespaces PASS");
}
static void rights_body(void)
{
    int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    key_t key=(key_t)(0x4e000000U+(unsigned)getpid());pid_t child=fork();CHECK(child>=0);
    union {struct cmsghdr align;unsigned char data[CMSG_SPACE(3*sizeof(int))];} control;
    if(!child) {
        CHECK(!close(pair[0])&&!unshare(ALL_NAMESPACES));set_names("rights-private","rights.domain");
        (void)semaphore(key,33);int descriptors[3];struct namespace_ids ids=current_ids();
        for(unsigned i=0;i<3;i++) {
            char path[80];snprintf(path,sizeof(path),"/proc/thread-self/ns/%s",namespace_names[i]);
            descriptors[i]=open(path,O_RDONLY);CHECK(descriptors[i]>=0);
        }
        memset(&control,0,sizeof(control));struct iovec iov={&ids,sizeof(ids)};
        struct msghdr message={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control)};
        struct cmsghdr *header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;
        header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(descriptors));
        memcpy(CMSG_DATA(header),descriptors,sizeof(descriptors));CHECK(sendmsg(pair[1],&message,0)==sizeof(ids));
        for(unsigned i=0;i<3;i++)CHECK(!close(descriptors[i]));
        _exit(0);
    }
    CHECK(!close(pair[1]));struct namespace_ids ids;memset(&control,0,sizeof(control));
    struct iovec iov={&ids,sizeof(ids)};struct msghdr message={.msg_iov=&iov,.msg_iovlen=1,
        .msg_control=control.data,.msg_controllen=sizeof(control)};
    CHECK(recvmsg(pair[0],&message,MSG_CMSG_CLOEXEC)==sizeof(ids)&&
        !(message.msg_flags&(MSG_CTRUNC|MSG_TRUNC)));
    struct cmsghdr *header=CMSG_FIRSTHDR(&message);CHECK(header&&header->cmsg_level==SOL_SOCKET&&
        header->cmsg_type==SCM_RIGHTS&&header->cmsg_len==CMSG_LEN(3*sizeof(int)));
    int descriptors[3];memcpy(descriptors,CMSG_DATA(header),sizeof(descriptors));
    wait_ok(child);CHECK(!close(pair[0]));
    const int types[]={CLONE_NEWNS,CLONE_NEWUTS,CLONE_NEWIPC};
    for(unsigned i=0;i<3;i++) {
        struct stat stat;CHECK(ioctl(descriptors[i],NS_GET_NSTYPE)==types[i]);
        CHECK((fcntl(descriptors[i],F_GETFD)&FD_CLOEXEC)&&!fstat(descriptors[i],&stat)&&stat.st_ino==ids.value[i]);
        CHECK(!setns(descriptors[i],types[i])&&!close(descriptors[i]));
    }
    CHECK(same_ids(ids,current_ids()));names("rights-private","rights.domain");
    int sem=semget(key,1,0);CHECK(sem>=0&&semctl(sem,0,GETVAL)==33&&!semctl(sem,0,IPC_RMID));
}
static void namespace_fd_rights(void)
{
    pid_t child=fork();CHECK(child>=0);if(!child){rights_body();_exit(0);}wait_ok(child);
    puts("UTS CASE namespace-rights-source-exit PASS");
}
int main(int argc,char **argv)
{
    if(argc==4&&!strcmp(argv[1],"--exec")){names(argv[2],argv[3]);return 0;}
    if(argc==5&&!strcmp(argv[1],"--namespace-exec")) {
        names("nonleader-private","nonleader.domain");struct namespace_ids ids=current_ids();
        CHECK(ids.value[1]==strtoull(argv[3],NULL,10)&&ids.value[2]==strtoull(argv[4],NULL,10));
        int sem=semget((key_t)strtoul(argv[2],NULL,10),1,0);
        CHECK(sem>=0&&semctl(sem,0,GETVAL)==39&&!semctl(sem,0,IPC_RMID));return 0;
    }
    CHECK(argc==1);
    int original=open("/proc/self/ns/uts",O_RDONLY);CHECK(original>=0);
    fd_contract();unshare_and_join(original);clone_fork_exec(argv[0],original);
    thread_and_permissions(original);CHECK(!close(original));
    combined_clone3_pidfd();same_group_ipc();nonleader_namespace_exec(argv[0]);namespace_fd_rights();
    puts("UTS NAMESPACE PASS cases=8");return 0;
}
