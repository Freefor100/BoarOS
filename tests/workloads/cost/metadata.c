/* Reuse the original-consumer lifecycle and per-command framing. */
#define main original_consumer_main
#include "consumer.c"
#undef main
#include <termios.h>

static void metadata_loop(unsigned mode, int held)
{
    int holder=held?open("/metadata-target",O_RDONLY):-1;
    if(held) CHECK(holder>=0);
    struct stat expected,actual;CHECK(stat("/metadata-target",&expected)==0);
    char name[48];snprintf(name,sizeof(name),"metadata-%s-%s",
        mode==0?"stat":mode==1?"fstat":"open",held?"held":"unheld");
    cost_begin();
    for(unsigned i=0;i<4096;i++){
        int fd=-1;
        if(mode==0) CHECK(stat("/metadata-target",&actual)==0);
        else if(mode==1) CHECK(fstat(holder,&actual)==0);
        else {fd=open("/metadata-target",O_RDONLY);CHECK(fd>=0 && fstat(fd,&actual)==0 && close(fd)==0);}
        CHECK(actual.st_ino==expected.st_ino && actual.st_mode==expected.st_mode && actual.st_size==expected.st_size);
    }
    cost_end(name,0,0,0,0);
    printf("COST METADATA WORK %s 4096\n",name);
    if(holder>=0)CHECK(close(holder)==0);
}
static void metadata_create(void)
{
    cost_begin();
    for(unsigned i=0;i<2048;i++){
        char name[64];snprintf(name,sizeof(name),"/metadata-empty-%u",i);
        int fd=open(name,O_CREAT|O_EXCL|O_RDWR,0640);CHECK(fd>=0);
        struct stat st;CHECK(fstat(fd,&st)==0 && st.st_size==0 && (st.st_mode&0777)==0640);
        CHECK(close(fd)==0 && unlink(name)==0);
    }
    cost_end("metadata-create",0,0,0,0);
    puts("COST METADATA WORK metadata-create 2048");
}
static void lmbench_command(unsigned libc, unsigned index)
{
    static const char *methods[]={"stat","fstat","open",0};
    const char *cwd=libc?"/glibc":"/musl";
    CHECK(chdir(cwd)==0);fflush(NULL);
    char name[48];snprintf(name,sizeof(name),"metadata-%s-lmbench-%u",libc?"glibc":"musl",index);
    puts("COST APPLICATION BEGIN");printf("COST APPLICATION ARGV %s ./lmbench_all %s",name,index==3?"lat_fs":"lat_syscall");
    if(index<3)printf(" -P 1 %s /var/tmp/lmbench",methods[index]);else printf(" /var/tmp");
    putchar('\n');fflush(NULL);cost_begin();
    unsigned long long start=nanoseconds();pid_t child=fork();CHECK(child>=0);
    if(!child){
        setenv("LD_LIBRARY_PATH",libc?"/glibc/lib":"/musl/lib",1);
        char *call[]={"./lmbench_all","lat_syscall","-P","1",(char *)methods[index],"/var/tmp/lmbench",0};
        char *fs[]={"./lmbench_all","lat_fs","/var/tmp",0};
        execv("./lmbench_all",index==3?fs:call);_exit(127);
    }
    int status;CHECK(waitpid(child,&status,0)==child);
    unsigned long long stop=nanoseconds();drain_directory();
    int directory=open("/var/tmp",O_RDONLY|O_DIRECTORY);CHECK(directory>=0 && fsync(directory)==0 && close(directory)==0);
    unsigned long long drained=nanoseconds();cost_end(name,0,0,0,0);
    printf("COST APPLICATION RESULT %s %d %llu %llu\n",name,status,stop-start,drained-stop);fflush(NULL);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);cost_init();
    CHECK(mkdir("/dev",0755)==0 || errno==EEXIST);
    CHECK(mknod("/dev/null",S_IFCHR|0666,makedev(1,3))==0 || errno==EEXIST);
    CHECK(mknod("/dev/zero",S_IFCHR|0666,makedev(1,5))==0 || errno==EEXIST);
    CHECK(mkdir("/lib",0755)==0 || errno==EEXIST);
    CHECK(mkdir("/tmp",01777)==0 || errno==EEXIST);
    CHECK(symlink("/musl/lib/libc.so","/lib/ld-musl-riscv64-sf.so.1")==0 || errno==EEXIST);
    CHECK(symlink("/glibc/lib/ld-linux-riscv64-lp64d.so.1","/lib/ld-linux-riscv64-lp64d.so.1")==0 || errno==EEXIST);
    CHECK(mkdir("/var",0755)==0 || errno==EEXIST);
    CHECK(mkdir("/var/tmp",0755)==0 || errno==EEXIST);
    int fd=open("/metadata-target",O_CREAT|O_RDWR|O_TRUNC,0644);CHECK(fd>=0);
    CHECK(write(fd,"metadata",8)==8 && fsync(fd)==0 && close(fd)==0);
    fd=open("/var/tmp/lmbench",O_CREAT|O_RDWR,0644);CHECK(fd>=0 && close(fd)==0);
    for(unsigned mode=0;mode<3;mode++){
        if(mode!=1)metadata_loop(mode,0);
        metadata_loop(mode,1);
    }
    metadata_create();
    for(unsigned libc=0;libc<2;libc++){
        for(unsigned i=0;i<4;i++)lmbench_command(libc,i);
        command(libc,1);
    }
    CHECK(chdir("/")==0);cost_begin();
    fd=open("/",O_RDONLY|O_DIRECTORY);CHECK(fd>=0 && fsync(fd)==0 && close(fd)==0);
    cost_end("metadata-final-sync",0,0,0,0);
    puts("COST PASS metadata");CHECK(fflush(NULL)==0);
    /* fflush 只清用户缓冲；等待 UART 排空，防止关机 raw 输出插入快照尾部。 */
    CHECK(tcdrain(STDOUT_FILENO)==0);
    if(access("/cost-linux",F_OK)==0)reboot(RB_POWER_OFF);
    return 0;
}
