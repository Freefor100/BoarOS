#define _GNU_SOURCE
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int ready_fd, block_fd;
static pthread_t leader;

static void *blocked(void *join_leader)
{
    if (join_leader) {
        if (pthread_join(leader, NULL)) _exit(80);
        if (write(ready_fd, "r", 1) != 1) _exit(81);
    }
    char byte;
    for (;;) (void)read(block_fd, &byte, 1);
}

static int reboot_error(long magic1,long magic2,long command,void *argument,int expected)
{
    errno=0;
    return syscall(SYS_reboot,magic1,magic2,command,argument)==-1 && errno==expected;
}

static int reboot_actions(int halt)
{
    const long magic1=0xfee1deadU,magic2=672274793;
    const unsigned commands[]={0,0x89abcdefU,0x45584543U,0xd000fce2U,UINT32_MAX};
    if(!reboot_error(0,magic2,0xa1b2c3d4U,(void *)-1,EINVAL) ||
       !reboot_error(magic1,0,0xa1b2c3d4U,(void *)-1,EINVAL)) return 90;
    for(unsigned i=0;i<sizeof(commands)/sizeof(commands[0]);i++)
        if(!reboot_error(magic1,magic2,commands[i],(void *)-1,EINVAL))return 91;
    long page=sysconf(_SC_PAGESIZE);
    char *guard=mmap(NULL,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(guard==MAP_FAILED || mprotect(guard+page,(size_t)page,PROT_NONE))return 92;
    memset(guard,'R',(size_t)page);
    if(!reboot_error(magic1,magic2,0xa1b2c3d4U,guard+page-16,EFAULT))return 93;
    pid_t unprivileged=fork();if(unprivileged<0)return 94;
    if(!unprivileged) {
        if(setuid(1001))_exit(95);
        _exit(reboot_error(0,0,0xa1b2c3d4U,(void *)-1,EPERM) ? 0 : 96);
    }
    int status;
    if(waitpid(unprivileged,&status,0)!=unprivileged || status)return 97;
    unsigned generation=0;
    int fd=open("/generation",O_RDONLY);
    if(fd>=0) {if(read(fd,&generation,sizeof(generation))!=sizeof(generation))return 98;close(fd);}
    if(generation>2 || (halt && generation))return 99;
    if(generation) {
        unsigned char data[513];fd=open("/persisted",O_RDONLY);if(fd<0)return 100;
        for(unsigned i=0;i<32791;) {
            ssize_t n=read(fd,data,sizeof(data));if(n<=0)return 101;
            for(ssize_t j=0;j<n;j++)if(data[j]!=(unsigned char)((i+j)*7+generation-1))return 102;
            i+=(unsigned)n;
        }
        if(read(fd,data,1))return 103;
        close(fd);
    }
    fd=open("/persisted",O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return 104;
    for(unsigned i=0;i<32791;) {
        unsigned char data[513];size_t n=32791-i;if(n>sizeof(data))n=sizeof(data);
        for(unsigned j=0;j<n;j++)data[j]=(unsigned char)((i+j)*7+generation);
        if(write(fd,data,n)!=(ssize_t)n)return 105;
        i+=(unsigned)n;
    }
    if(close(fd))return 106;
    fd=open("/generation",O_WRONLY|O_CREAT|O_TRUNC,0600);unsigned next=generation+1;
    if(fd<0 || write(fd,&next,sizeof(next))!=sizeof(next) || close(fd))return 107;
    int ready[2],hold[2];if(pipe(ready) || pipe(hold))return 108;
    pid_t stopped=fork();if(stopped<0)return 109;
    if(!stopped) {
        close(ready[0]);
        block_fd=hold[0];
        if(setsid()<0 || open("/data",O_RDONLY)<0 || socket(AF_INET,SOCK_STREAM,0)<0)_exit(110);
        if(write(ready[1],"r",1)!=1)_exit(111);
        raise(SIGSTOP);blocked(NULL);
    }
    close(ready[1]);char byte;
    if(read(ready[0],&byte,1)!=1 || waitpid(stopped,&status,WUNTRACED)!=stopped || !WIFSTOPPED(status))return 112;
    printf("REBOOT READY generation=%u action=%s invalid=PASS persistence=PASS stopped=PASS pid1=live\n",
        generation,halt ? "halt" : generation==0 ? "restart" : generation==1 ? "restart2" : "poweroff");
    fflush(stdout);
    pid_t requester=fork();if(requester<0)return 113;
    if(!requester) {
        const unsigned action=halt ? 0xcdef0123U : generation==0 ? 0x01234567U : generation==1 ? 0xa1b2c3d4U : 0x4321fedcU;
        const unsigned magics[]={672274793,85072278,537993216};
        /* RESTART2 must truncate at byte 255 without reading the guard page. */
        syscall(SYS_reboot,(long)(UINT64_C(0x1234000000000000)|magic1),
            (long)(UINT64_C(0x5678000000000000)|magics[generation]),
            (long)(UINT64_C(0x9abc000000000000)|action),
            generation==1 ? guard+page-255 : (void *)-1);
        puts("REBOOT RETURNED");fflush(stdout);_exit(114);
    }
    close(hold[0]);
    /* PID 1 stays blocked with the write owner live until root cleanup kills it. */
    for(;;) pause();
}

int main(void)
{
    char mode[16] = {0};
    int fd = open("/mode", O_RDONLY);
    if (fd < 0 || read(fd, mode, sizeof(mode) - 1) <= 0) return 70;
    close(fd);
    if(!strcmp(mode,"reboot") || !strcmp(mode,"halt"))return reboot_actions(!strcmp(mode,"halt"));
    int ready[2], hold[2];
    if (pipe(ready) || pipe(hold)) return 71;
    pid_t child = fork();
    if (child < 0) return 72;
    if (!child) {
        close(ready[0]);
        if (setsid() < 0 || open("/data", O_RDONLY) < 0) _exit(73);
        int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0 || listen(socket_fd, 1)) _exit(74);
        ready_fd = ready[1]; block_fd = hold[0];
        /* 保留写端，使PID 1退出不会通过EOF自然结束这个后台owner。 */
        if (!strcmp(mode, "threads")) {
            pthread_t workers[2]; leader = pthread_self();
            if (pthread_create(&workers[0], NULL, blocked, NULL) ||
                pthread_create(&workers[1], NULL, blocked, (void *)1)) _exit(75);
            pthread_exit(NULL);
        }
        if (write(ready_fd, "r", 1) != 1) _exit(76);
        if (!strcmp(mode, "stopped")) raise(SIGSTOP);
        if (!strcmp(mode, "forking")) {
            for (unsigned i = 0; i < 8; i++) {
                pid_t descendant = fork();
                if (descendant < 0) _exit(77);
                if (!descendant) blocked(NULL);
            }
        }
        blocked(NULL);
    }
    close(ready[1]);
    char byte;
    if (read(ready[0], &byte, 1) != 1) return 78;
    if (!strcmp(mode, "stopped")) {
        int status;
        if (waitpid(child, &status, WUNTRACED) != child || !WIFSTOPPED(status))
            return 79;
    }
    printf("INIT SHUTDOWN READY %s\n", mode);
    fflush(stdout);
    return 37;
}
