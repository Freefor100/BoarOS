#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
static _Thread_local int tls=83;
static volatile sig_atomic_t seen;
static void handler(int signal){(void)signal;seen++;}
static void *peer(void *pointer)
{char byte;return read(*(int *)pointer,&byte,1)==1 && tls==83 ? 0 : (void *)1;}
static int check(const char *path,int expected)
{
    int channel[2];pthread_t thread;void *result;
    int fd=open("/original",O_RDONLY|O_CLOEXEC);if(fd<0 || pipe(channel) || pthread_create(&thread,0,peer,&channel[0]))return 1;
    tls=97;pid_t pid=getpid();
    struct sigaction action={.sa_handler=handler};if(sigaction(SIGUSR1,&action,0))return 2;
    char *args[]={(char *)path,0};char *env[]={0};errno=0;
    int returned=execve(path,args,env);int error=errno;
    if(returned!=-1 || error!=expected){fprintf(stderr,"LA exec path=%s error=%d expected=%d\n",path,error,expected);return 3;}
    if(getpid()!=pid || tls!=97 ||
       fcntl(fd,F_GETFD)!=FD_CLOEXEC || lseek(fd,0,SEEK_CUR)!=0 || raise(SIGUSR1) || seen!=1)return 3;
    if(write(channel[1],"x",1)!=1 || pthread_join(thread,&result) || result ||
       close(channel[0]) || close(channel[1]) || close(fd))return 4;
    return 0;
}
int main(void)
{
    if(mkdir("/lib",0755) && errno!=EEXIST)return 1;
    int input=open("/loader-source",O_RDONLY),output=open("/lib/noexec.so.1",O_CREAT|O_TRUNC|O_WRONLY,0644);
    if(input<0 || output<0)return 1;
    char buffer[16384];ssize_t count;
    while((count=read(input,buffer,sizeof(buffer)))>0)if(write(output,buffer,count)!=count)return 1;
    if(count || close(input) || close(output))return 1;
    const struct {const char *path;int error;} cases[]={
        {"/bad-missing",ENOENT},{"/bad-noexec",EACCES},{"/bad-wrong",ELIBBAD},{"/bad-broken",ELIBBAD},{"/bad-short",EIO}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        pid_t child=fork();if(child<0)return 1;if(!child)_exit(check(cases[i].path,cases[i].error));
        int status;if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr,"LA interpreter error case=%u status=%x\n",i,status);return 2;
        }
    }
    puts("LA interpreter errno/old image/TLS/threads/fd/signal preserved");return 0;
}
