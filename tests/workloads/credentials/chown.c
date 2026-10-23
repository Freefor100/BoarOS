#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(int okay,const char *operation)
{
    if (okay) return;
    printf("CRED CHOWN FAIL %s errno=%d\n",operation,errno);
    fflush(stdout);
    _exit(1);
}

static const struct {mode_t mode;gid_t before,after;mode_t expected;} cases[]={
    {06600,100,200,0600}, {02600,100,200,02600},
    {06600,100,100,02600}, {02600,200,100,0600},
};

static void one(unsigned backend,unsigned index)
{
    int fd,other=-1;
    if (backend==2) {
        int ends[2];check(!pipe(ends),"pipe");fd=ends[0];other=ends[1];
    } else {
        fd=open(backend?"/chown-tmpfs/file":"/chown-file",O_CREAT|O_RDWR|O_TRUNC,0600);
        check(fd>=0,"open");
    }
    check(!fchown(fd,1000,cases[index].before),"initial-owner");
    check(!fchmod(fd,cases[index].mode),"initial-mode");
    check(!setgroups(0,NULL)&&!setresgid(100,100,100),"groups");
    check(!prctl(PR_SET_KEEPCAPS,1,0,0,0)&&!setresuid(1000,1000,1000),"uid");
    struct {uint32_t version;int32_t pid;} header={0x20080522,0};
    struct {uint32_t effective,permitted,inheritable;} data[2]={{1,1,0},{0,0,0}};
    check(!syscall(SYS_capset,&header,data),"capset-chown-only");
    check(!fchown(fd,(uid_t)-1,cases[index].after),"chown");
    struct stat st;check(!fstat(fd,&st),"fstat");
    printf("CRED CHOWN backend=%u case=%u mode=%o uid=%u gid=%u\n",backend,index,
           st.st_mode&07777,(unsigned)st.st_uid,(unsigned)st.st_gid);
    check((st.st_mode&07777)==cases[index].expected&&st.st_uid==1000&&
          st.st_gid==cases[index].after,"final-attributes");
    check(!close(fd)&&(other<0||!close(other)),"close");
}

int main(void)
{
    check(!mkdir("/chown-tmpfs",0700),"mkdir");
    check(!mount("none","/chown-tmpfs","tmpfs",0,NULL),"mount-tmpfs");
    for (unsigned backend=0;backend<3;backend++) for (unsigned index=0;index<4;index++) {
        fflush(stdout);pid_t pid=fork();check(pid>=0,"fork");
        if (!pid) {one(backend,index);fflush(stdout);_exit(0);}
        int status;check(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status),"child");
    }
    check(!umount("/chown-tmpfs")&&!rmdir("/chown-tmpfs"),"unmount");
    check(!unlink("/chown-file"),"unlink");
    puts("CRED CHOWN END 12 failures=0");return 0;
}
