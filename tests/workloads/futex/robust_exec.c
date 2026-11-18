#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){dprintf(2,"ROBUST EXEC FAIL %d %s errno=%d\n",__LINE__,#x,errno);_exit(1);}}while(0)
struct list {struct list *next;};
struct head {struct list list;long offset;struct list *pending;};
static struct shared {struct list node;uint32_t word;pid_t owner,leader;} *data;
static const char *executable;
extern char **environ;
static void *replace(void *unused)
{
    (void)unused;
    struct head head={.list.next=&data->node,.offset=offsetof(struct shared,word)-offsetof(struct shared,node)};
    data->node.next=&head.list;
    data->owner=(pid_t)syscall(SYS_gettid);data->word=(uint32_t)data->owner;
    CHECK(data->owner!=data->leader);
    CHECK(!syscall(SYS_set_robust_list,&head,sizeof(head)));
    char *args[]={(char *)executable,"--done",NULL};
    execve(executable,args,environ);CHECK(0);return NULL;
}
static int child(void *unused)
{
    (void)unused;data->leader=getpid();pthread_t thread;
    CHECK(!pthread_create(&thread,NULL,replace,NULL));
    for(;;)pause();
}
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"--done"))return 0;
    int nested=0,retained=0;
    for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--nested"))nested=1;else if(!strcmp(argv[i],"--expect-retained"))retained=1;else CHECK(0);}
    executable=argv[0];
    data=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(data!=MAP_FAILED);
    pid_t pid;void *stack=NULL;
    if(nested){stack=malloc(1U<<20);CHECK(stack);pid=clone(child,(char *)stack+(1U<<20),CLONE_NEWPID|SIGCHLD,NULL);}
    else {pid=fork();if(!pid)return child(NULL);}
    CHECK(pid>0);int status;
    CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));
    CHECK(data->owner!=data->leader);
    CHECK(data->word==(retained?(uint32_t)data->owner:UINT32_C(0x40000000)));
    if(nested)CHECK(data->leader==1&&data->owner==2);
    CHECK(!munmap(data,4096));free(stack);
    puts(retained?"ROBUST EXEC OBSERVED retained old TID (not OWNER_DIED)":
        nested?"ROBUST EXEC PASS nested nonleader old-local-TID cleanup":"ROBUST EXEC PASS nonleader old-TID cleanup");
    return 0;
}
