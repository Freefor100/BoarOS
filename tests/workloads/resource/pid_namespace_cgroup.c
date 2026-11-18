#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){dprintf(2,"PIDNS CGROUP FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);_exit(1);}}while(0)
static int nested,which,files[2],start[2][2],done[2][2],finish[2][2],writefd=-1;
static char directory[256];
static void send_byte(int fd){CHECK(write(fd,"x",1)==1);}
static void receive_byte(int fd){char c;CHECK(read(fd,&c,1)==1);}
static void listing(int fd,int child)
{
    CHECK(lseek(fd,0,SEEK_SET)==0);char buf[512];long value=0;int digits=0,ones=0,zeros=0,rows=0;ssize_t n;
    while((n=read(fd,buf,sizeof(buf)))>0)for(ssize_t i=0;i<n;i++){
        if(buf[i]=='\n'){
            CHECK(digits);rows++;if(!value)zeros++;if(value==getpid())ones++;
            if(child&&nested)CHECK(value==0||value==1);
            value=0;digits=0;
        }else{CHECK(buf[i]>='0'&&buf[i]<='9');value=value*10+buf[i]-'0';digits++;CHECK(digits<=10);}
    }
    CHECK(n==0&&!digits&&rows&&ones==1);
    if(child&&nested)CHECK(zeros>0);
}
static int child(void *unused)
{
    (void)unused;receive_byte(start[which][0]);if(nested)CHECK(getpid()==1);
    const char *names[]={"cgroup.procs","cgroup.threads"};
    for(unsigned i=0;i<2;i++){
        /* Parent already read this same OFD. Rewind regenerates in the current
         * reader's view; this is not the /proc/sysvipc opener-view contract. */
        listing(files[i],1);char path[320];CHECK(snprintf(path,sizeof(path),"%s/%s",directory,names[i])>0);
        int fd=open(path,O_RDONLY);CHECK(fd>=0);listing(fd,1);CHECK(!close(fd));
    }
    if(writefd>=0){char value[32];int n=snprintf(value,sizeof(value),"%d",getpid());CHECK(write(writefd,value,(size_t)n)==n);}
    send_byte(done[which][1]);receive_byte(finish[which][0]);return 0;
}
int main(int argc,char **argv)
{
    nested=argc==3&&!strcmp(argv[1],"--nested");int mounted=!nested;
    CHECK(snprintf(directory,sizeof(directory),"%s",nested?argv[2]:"/pidns-cgroup")>0);
    if(mounted){CHECK(!mkdir(directory,0755)||errno==EEXIST);CHECK(!mount("none",directory,"cgroup2",0,NULL));}
    const char *names[]={"cgroup.procs","cgroup.threads"};char path[320];
    for(unsigned i=0;i<2;i++){CHECK(snprintf(path,sizeof(path),"%s/%s",directory,names[i])>0);files[i]=open(path,O_RDONLY);CHECK(files[i]>=0);listing(files[i],0);}
    CHECK(snprintf(path,sizeof(path),"%s/cgroup.procs",directory)>0);writefd=open(path,O_WRONLY);
    if(writefd<0){CHECK(nested&&(errno==EACCES||errno==EROFS||errno==EPERM));puts("PIDNS CGROUP native migration unavailable: opener permission");}
    char *stack[2];pid_t pid[2];
    for(unsigned i=0;i<2;i++){CHECK(!pipe(start[i])&&!pipe(done[i])&&!pipe(finish[i]));stack[i]=malloc(65536);CHECK(stack[i]);}
    for(which=0;which<2;which++){pid[which]=clone(child,stack[which]+65536,SIGCHLD|(nested?CLONE_NEWPID:0),NULL);CHECK(pid[which]>0);}
    for(unsigned i=0;i<2;i++){send_byte(start[i][1]);receive_byte(done[i][0]);}
    for(unsigned i=0;i<2;i++)send_byte(finish[i][1]);
    for(unsigned i=0;i<2;i++){int status;CHECK(waitpid(pid[i],&status,0)==pid[i]&&WIFEXITED(status)&&!WEXITSTATUS(status));free(stack[i]);for(unsigned j=0;j<2;j++){close(start[i][j]);close(done[i][j]);close(finish[i][j]);}}
    for(unsigned i=0;i<2;i++){listing(files[i],0);CHECK(!close(files[i]));}if(writefd>=0)CHECK(!close(writefd));
    if(mounted){CHECK(!umount(directory));CHECK(!rmdir(directory));}
    puts(nested?"PIDNS CGROUP PASS nested current-reader inherited FD zero rows sibling identities":"PIDNS CGROUP PASS current-reader inherited FD and migration");return 0;
}
