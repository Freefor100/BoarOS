#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define ALL_CAPS ((UINT64_C(1)<<41)-1)
#define ONE_CAP (UINT64_C(1)<<10)
struct cap_header{uint32_t version;int32_t pid;};
struct cap_data{uint32_t e,p,i;};
struct snapshot {
    uint32_t uid[4],gid[4];uint64_t effective,permitted,inheritable;
    uint64_t aux[5];int secure,dumpable,nnp,bits,ambient,preloaded,safe_environment,preload_visible,cloexec;
};
static int fd_output,helper_pid;
static int control_pipe[2];
static char fs_stack[32768] __attribute__((aligned(16)));
static void check(int okay,const char *name)
{if(!okay){printf("CRED EXEC FAIL %s errno=%d\n",name,errno);fflush(stdout);_exit(99);}}
static void setcaps(uint64_t e,uint64_t p,uint64_t i)
{
    struct cap_header h={0x20080522,0};struct cap_data d[2]={{(uint32_t)e,(uint32_t)p,(uint32_t)i},{(uint32_t)(e>>32),(uint32_t)(p>>32),(uint32_t)(i>>32)}};
    check(!syscall(SYS_capset,&h,d),"capset");
}
static struct snapshot take(void)
{
    struct snapshot s={0};struct cap_header h={0x20080522,0};struct cap_data d[2]={{0}};
    check(!syscall(SYS_getresuid,&s.uid[0],&s.uid[1],&s.uid[2]),"getresuid");s.uid[3]=(uint32_t)syscall(SYS_setfsuid,-1);
    check(!syscall(SYS_getresgid,&s.gid[0],&s.gid[1],&s.gid[2]),"getresgid");s.gid[3]=(uint32_t)syscall(SYS_setfsgid,-1);
    check(!syscall(SYS_capget,&h,d),"capget");
    s.effective=d[0].e|((uint64_t)d[1].e<<32);s.permitted=d[0].p|((uint64_t)d[1].p<<32);s.inheritable=d[0].i|((uint64_t)d[1].i<<32);
    s.aux[0]=getauxval(AT_UID);s.aux[1]=getauxval(AT_EUID);s.aux[2]=getauxval(AT_GID);s.aux[3]=getauxval(AT_EGID);s.aux[4]=getauxval(AT_SECURE);
    s.secure=(int)s.aux[4];s.dumpable=prctl(PR_GET_DUMPABLE);s.nnp=prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0);s.bits=prctl(PR_GET_SECUREBITS);
    s.ambient=prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_IS_SET,10,0,0);
    s.preloaded=getenv("CRED_PRELOADED")!=0;s.preload_visible=getenv("LD_PRELOAD")!=0;
    s.safe_environment=secure_getenv("CRED_KEEP")!=0;
    errno=0;s.cloexec=fcntl(100,F_GETFD)==-1&&errno==EBADF;
    return s;
}
static int probe(int argc,char **argv)
{
    check(argc>=3,"probe-args");int fd=atoi(argv[2]);
    struct snapshot s=take();check(s.uid[0]==s.aux[0]&&s.uid[1]==s.aux[1]&&s.gid[0]==s.aux[2]&&s.gid[1]==s.aux[3],"auxv-identity");
    check(s.preloaded==!!(getauxval(AT_BASE)&&!s.secure),"loader-preload-selection");
    check(s.safe_environment==!s.secure,"secure-getenv");
    check(write(fd,&s,sizeof(s))==(ssize_t)sizeof(s),"probe-output");
    if(argc>3){pid_t helper=(pid_t)atoi(argv[3]);if(helper){int status;check(waitpid(helper,&status,0)==helper&&!status,"shared-fs-helper-exit");}}
    return 0;
}
enum mode{PLAIN,NO_NEW,NO_ROOT,AMBIENT,BOUND_DROP,FSUID,NONLEADER,SHARED_FS,WRITE_LIMIT,TRUNCATE_LIMIT,FALLOCATE_LIMIT};
struct scenario{const char *name,*path;enum mode mode;unsigned euid,egid;uint64_t p,e,i;int ambient,secure,dump,error;};
static void drop(enum mode mode)
{
    check(!syscall(SYS_setgroups,0,0),"drop-groups");
    check(!syscall(SYS_setresgid,1000,1000,1000),"drop-gid");
    if(mode==NO_ROOT)check(!prctl(PR_SET_SECUREBITS,1),"noroot");
    if(mode==BOUND_DROP)check(!prctl(PR_CAPBSET_DROP,10),"bounding-drop");
    if(mode==AMBIENT||mode==FSUID)check(!prctl(PR_SET_KEEPCAPS,1),"keepcaps");
    check(!syscall(SYS_setresuid,1000,1000,1000),"drop-uid");
    if(mode==AMBIENT){setcaps(ONE_CAP,ONE_CAP,ONE_CAP);check(!prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_RAISE,10,0,0),"ambient-raise");}
    if(mode==FSUID){setcaps(ALL_CAPS,ALL_CAPS,0);check(syscall(SYS_setfsuid,2000)==1000,"fsuid-change");}
    if(mode==NO_NEW)check(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0),"nnp");
}
static void execute(const struct scenario *c)
{
    char fd[20],helper[20];snprintf(fd,sizeof(fd),"%d",fd_output);snprintf(helper,sizeof(helper),"%d",helper_pid);
    char *const argv[]={(char *)c->path,"--probe",fd,helper,0};
    char *const env[]={"LD_PRELOAD=/cred/preload.so","LD_LIBRARY_PATH=/cred/untrusted","CRED_KEEP=1",0};
    struct snapshot before=take();
    execve(c->path,argv,env);int error=errno;
    struct snapshot after=take();check(error==c->error,"exec-errno");
    check(!memcmp(&before,&after,sizeof(before)),"failed-exec-preserves-state");
    check(write(fd_output,&error,sizeof(error))==(ssize_t)sizeof(error),"failed-exec-report");_exit(0);
}
static void limit_failure(const struct scenario *c)
{
    uint64_t limit[2]={0,UINT64_MAX};check(!syscall(SYS_prlimit64,0,1,limit,0),"fsize-limit");
    check(signal(SIGXFSZ,SIG_IGN)!=SIG_ERR,"ignore-xfsz");
    int fd=open(c->path,O_WRONLY);check(fd>=0,"killpriv-open");errno=0;
    struct stat st;check(!fstat(fd,&st),"killpriv-before");
    long result=c->mode==WRITE_LIMIT?write(fd,"x",1):c->mode==TRUNCATE_LIMIT?ftruncate(fd,st.st_size+1):syscall(SYS_fallocate,fd,0,st.st_size,4096);
    check(result==-1&&errno==EFBIG,"fsize-error");check(!fstat(fd,&st),"killpriv-stat");
    unsigned expected=c->mode==FALLOCATE_LIMIT?0777:06777;
    if((st.st_mode&07777)!=expected){printf("CRED EXEC mode %s actual=%o expected=%o\n",c->name,st.st_mode&07777,expected);check(0,"killpriv-mode");}
    check(!close(fd),"killpriv-close");
}
static int fs_helper(void *unused)
{(void)unused;close(control_pipe[1]);char b;check(read(control_pipe[0],&b,1)==0,"cloexec-releases-helper");return 0;}
static void *exec_worker(void *opaque)
{const struct scenario *c=opaque;drop(PLAIN);execute(c);return 0;}
static void run(const struct scenario *c)
{
    int pipefd[2];check(!pipe(pipefd),"pipe");fflush(stdout);pid_t child=fork();check(child>=0,"fork");
    if(!child){close(pipefd[0]);fd_output=pipefd[1];helper_pid=0;
        int fd=open("/cred/ordinary",O_RDONLY);check(fd>=0&&dup3(fd,100,O_CLOEXEC)==100,"cloexec-fd");close(fd);
        if(c->mode==NONLEADER){pthread_t t;check(!pthread_create(&t,0,exec_worker,(void *)c),"nonleader-create");for(;;)pause();}
        drop(c->mode);
        if(c->mode==WRITE_LIMIT||c->mode==TRUNCATE_LIMIT||c->mode==FALLOCATE_LIMIT)limit_failure(c);
        if(c->mode==SHARED_FS){check(!pipe2(control_pipe,O_CLOEXEC),"fs-control-pipe");
            helper_pid=clone(fs_helper,fs_stack+sizeof(fs_stack),CLONE_FS|SIGCHLD,0);check(helper_pid>0,"clone-fs");close(control_pipe[0]);}
        execute(c);
    }
    close(pipefd[1]);unsigned char bytes[sizeof(struct snapshot)];size_t done=0;
    while(done<sizeof(bytes)){ssize_t r=read(pipefd[0],bytes+done,sizeof(bytes)-done);check(r>=0,"report-read");if(!r)break;done+=(size_t)r;}
    close(pipefd[0]);int status=0;check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status),c->name);
    if(c->error){int actual=0;check(done==sizeof(actual),"error-report-size");memcpy(&actual,bytes,sizeof(actual));check(actual==c->error,"error-report-value");}
    else{struct snapshot s;check(done==sizeof(s),"snapshot-size");memcpy(&s,bytes,sizeof(s));
        if(s.uid[0]!=1000||s.uid[1]!=c->euid||s.uid[2]!=c->euid||s.uid[3]!=c->euid||
           s.gid[0]!=1000||s.gid[1]!=c->egid||s.gid[2]!=c->egid||s.gid[3]!=c->egid||
           s.permitted!=c->p||s.effective!=c->e||s.inheritable!=c->i||s.ambient!=c->ambient||s.secure!=c->secure||s.dumpable!=c->dump||!s.cloexec){
            printf("CRED EXEC actual %s uid=%u/%u/%u/%u gid=%u/%u/%u/%u P=%llx E=%llx I=%llx A=%d secure=%d dump=%d cloexec=%d\n",
                c->name,s.uid[0],s.uid[1],s.uid[2],s.uid[3],s.gid[0],s.gid[1],s.gid[2],s.gid[3],
                (unsigned long long)s.permitted,(unsigned long long)s.effective,(unsigned long long)s.inheritable,s.ambient,s.secure,s.dumpable,s.cloexec);check(0,"exec-result");}
        check(s.nnp==(c->mode==NO_NEW)&&!(s.bits&16),"exec-control-state");
        printf("CRED EXEC env %s LD_PRELOAD-visible=%d loaded=%d safe-getenv=%d\n",c->name,s.preload_visible,s.preloaded,s.safe_environment);
    }
    printf("CRED EXEC CASE %s PASS\n",c->name);fflush(stdout);
}
static void nosuid_mount(void)
{
    check(!mkdir("/cred/nosuid",0755)||errno==EEXIST,"nosuid-dir");
    check(!mount("none","/cred/nosuid","tmpfs",MS_NOSUID,0),"nosuid-mount");
    int input=open("/cred/ordinary",O_RDONLY),output=open("/cred/nosuid/suid",O_CREAT|O_WRONLY,0755);check(input>=0&&output>=0,"nosuid-open");
    char data[4096];for(;;){ssize_t got=read(input,data,sizeof(data));check(got>=0,"nosuid-read");if(!got)break;check(write(output,data,(size_t)got)==got,"nosuid-write");}
    check(!fchmod(output,04755)&&!close(output)&&!close(input),"nosuid-close");
}
static void identity_links(void)
{
    check(!mkdir("/cred/linktest",0700),"link-directory");
    check(!chown("/cred/linktest",1000,1000),"link-directory-owner");
    fflush(stdout);pid_t child=fork();check(child>=0,"link-fork");
    if(!child){
        drop(PLAIN);int fd=open("/cred/linktest/source",O_CREAT|O_RDWR,0600);check(fd>=0,"link-open");
        check(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0),"link-nnp");
        check(!linkat(fd,"",AT_FDCWD,"/cred/linktest/after-nnp",AT_EMPTY_PATH),"nnp-keeps-open-credentials");
        printf("CRED EXEC CASE nnp-keeps-open-credentials PASS\n");fflush(stdout);
        pid_t grandchild=fork();check(grandchild>=0,"link-grandchild");
        if(!grandchild){errno=0;check(linkat(fd,"",AT_FDCWD,"/cred/linktest/after-fork",AT_EMPTY_PATH)==-1&&errno==ENOENT,"fork-has-new-credentials");_exit(0);}
        int status;check(waitpid(grandchild,&status,0)==grandchild&&!status,"link-grandchild-exit");
        printf("CRED EXEC CASE fork-has-new-credentials PASS\n");fflush(stdout);_exit(0);
    }
    int status;check(waitpid(child,&status,0)==child&&!status,"identity-links-exit");
}
int main(int argc,char **argv)
{
    if(argc>1&&!strcmp(argv[1],"--probe"))return probe(argc,argv);
    static const struct scenario cases[]={
        {"ordinary","/cred/ordinary",PLAIN,1000,1000,0,0,0,0,0,1,0},
        {"suid-root","/cred/suid-root",PLAIN,0,1000,ALL_CAPS,ALL_CAPS,0,0,1,0,0},
        {"suid-user","/cred/suid-user",PLAIN,1001,1000,0,0,0,0,1,0,0},
        {"sgid","/cred/sgid",PLAIN,1000,2001,0,0,0,0,1,0,0},
        {"sgid-no-group-exec","/cred/sgid-no-group-exec",PLAIN,1000,1000,0,0,0,0,0,1,0},
        {"filecap-rev1","/cred/cap1",PLAIN,1000,1000,ONE_CAP,ONE_CAP,0,0,1,0,0},
        {"filecap-rev2","/cred/cap2",PLAIN,1000,1000,ONE_CAP,ONE_CAP,0,0,1,0,0},
        {"filecap-rev3","/cred/cap3",PLAIN,1000,1000,ONE_CAP,ONE_CAP,0,0,1,0,0},
        {"filecap-external","/cred/cap-external",PLAIN,1000,1000,ONE_CAP,ONE_CAP,0,0,1,0,0},
        {"filecap-other-rootid","/cred/cap-rootid",PLAIN,1000,1000,0,0,0,0,0,1,0},
        {"filecap-malformed","/cred/cap-bad",PLAIN,0,0,0,0,0,0,0,0,EINVAL},
        {"filecap-bounding","/cred/cap2",BOUND_DROP,0,0,0,0,0,0,0,0,EPERM},
        {"nnp-suid","/cred/suid-root",NO_NEW,1000,1000,0,0,0,0,0,1,0},
        {"nnp-filecap","/cred/cap2",NO_NEW,1000,1000,0,0,0,0,1,1,0},
        {"noroot-suid","/cred/suid-root",NO_ROOT,0,1000,0,0,0,0,1,0,0},
        {"ambient-ordinary","/cred/ordinary",AMBIENT,1000,1000,ONE_CAP,ONE_CAP,ONE_CAP,1,0,1,0},
        {"ambient-zero-filecap","/cred/cap-zero",AMBIENT,1000,1000,0,0,ONE_CAP,0,0,1,0},
        {"fsuid-reset","/cred/ordinary",FSUID,1000,1000,0,0,0,0,0,0,0},
        {"nonleader-exec","/cred/ordinary",NONLEADER,1000,1000,0,0,0,0,0,1,0},
        {"shared-fs-suid","/cred/suid-root",SHARED_FS,1000,1000,0,0,0,0,1,1,0},
        {"nosuid-mount","/cred/nosuid/suid",PLAIN,1000,1000,0,0,0,0,0,1,0},
        {"write-limit-keeps-privilege","/cred/limit-write",WRITE_LIMIT,1000,1000,ONE_CAP,ONE_CAP,0,0,1,0,0},
        {"truncate-limit-keeps-privilege","/cred/limit-truncate",TRUNCATE_LIMIT,1000,1000,ONE_CAP,ONE_CAP,0,0,1,0,0},
        {"fallocate-limit-clears-privilege","/cred/limit-fallocate",FALLOCATE_LIMIT,1000,1000,0,0,0,0,0,1,0},
    };
    nosuid_mount();
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++)run(&cases[i]);
    identity_links();
    check(!umount("/cred/nosuid"),"nosuid-unmount");
    printf("CRED EXEC END %zu\n",sizeof(cases)/sizeof(cases[0])+2);return 0;
}
