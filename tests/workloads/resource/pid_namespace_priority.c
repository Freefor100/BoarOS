#define _GNU_SOURCE
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int nested;
static int go[2][2],done[2][2];
static void token(int fd,int write_it){char c='x';assert((write_it?write(fd,&c,1):read(fd,&c,1))==1);}
static int io(int set,int which,int who,int value){return syscall(set?SYS_ioprio_set:SYS_ioprio_get,which,who,value);}
static int nice_get(int which,int who){errno=0;int n=getpriority(which,who);assert(!errno);return n;}
static int child(void *arg)
{
    int i=(int)(long)arg;
    if(nested)assert(getpid()==1);
    token(go[i][0],0);
    if(!i){
        assert(!setpriority(PRIO_PROCESS,getpid(),5));
        assert(nice_get(PRIO_PROCESS,getpid())==5);
        assert(!io(1,1,getpid(),(2<<13)|6));assert(io(0,1,getpid(),0)==((2<<13)|6));
        if(nested){
            assert(nice_get(PRIO_USER,getuid())==5);
            assert(nice_get(PRIO_PGRP,0)==0);
            assert(io(0,3,getuid(),0)==((2<<13)|6));
            assert(io(0,2,0,0)==((2<<13)|4));
            errno=0;assert(getpriority(PRIO_PGRP,1)==-1&&errno==ESRCH);
            errno=0;assert(io(0,2,1,0)==-1&&errno==ESRCH);
            assert(!setpriority(PRIO_USER,getuid(),6));
            assert(!io(1,3,getuid(),(2<<13)|7));
        }
        token(done[i][1],1);token(go[i][0],0);
        /* Zero PGID selects the inherited real group even when its number
         * is invisible. This session was created for this workload only. */
        assert(!setpriority(PRIO_PGRP,0,7));assert(!io(1,2,0,(2<<13)|5));
        token(done[i][1],1);
    }else{
        assert(nice_get(PRIO_PROCESS,0)==0);assert(io(0,1,0,0)==0);
        token(done[i][1],1);token(go[i][0],0);
        assert(nice_get(PRIO_PROCESS,0)==7);assert(io(0,1,0,0)==((2<<13)|5));
        token(done[i][1],1);
    }
    return 0;
}
int main(int argc,char **argv)
{
    nested=argc==2&&!strcmp(argv[1],"--nested");assert(argc==1||nested);
    if(setsid()<0)assert(errno==EPERM&&getsid(0)==getpid()&&getpgrp()==getpid());
    assert(nice_get(PRIO_PROCESS,0)==0);
    pid_t children[2];void *stack[2];
    for(int i=0;i<2;i++){assert(!pipe(go[i])&&!pipe(done[i]));stack[i]=malloc(65536);assert(stack[i]);children[i]=clone(child,(char *)stack[i]+65536,SIGCHLD|(nested?CLONE_NEWPID:0),(void *)(long)i);assert(children[i]>0);}
    token(go[0][1],1);token(done[0][0],0);
    assert(nice_get(PRIO_PROCESS,0)==0&&io(0,1,0,0)==0);
    token(go[1][1],1);token(done[1][0],0);
    token(go[0][1],1);token(done[0][0],0);
    assert(nice_get(PRIO_PROCESS,0)==7&&io(0,1,0,0)==((2<<13)|5));
    token(go[1][1],1);token(done[1][0],0);
    for(int i=0;i<2;i++){int status;assert(waitpid(children[i],&status,0)==children[i]&&WIFEXITED(status)&&!WEXITSTATUS(status));free(stack[i]);for(int j=0;j<2;j++){assert(!close(go[i][j]));assert(!close(done[i][j]));}}
    puts(nested?"PIDNS PRIORITY PASS nested sibling UID isolation and inherited group":"PIDNS PRIORITY PASS process and inherited group");return 0;
}
