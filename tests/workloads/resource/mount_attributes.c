#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){dprintf(2,"MOUNT-ATTR FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1);}}while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static size_t page;
static unsigned cases;
static void pass(const char *name){printf("MOUNT-ATTR CASE %s PASS\n",name);cases++;}
static void directory(const char *path){CHECK(!mkdir(path,0755)||errno==EEXIST);}
static void joined(pid_t child,int code){int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==code);}
static void token(int fd){CHECK(write(fd,"x",1)==1);}
static void expect(int fd){char ch;CHECK(read(fd,&ch,1)==1&&ch=='x');}
static void fdname(char path[64],int fd){CHECK(snprintf(path,64,"/ma/proc/self/fd/%d",fd)>0);}
static void copy_file(const char *source,const char *target)
{
    int in=open(source,O_RDONLY),out=open(target,O_CREAT|O_TRUNC|O_WRONLY,0755);CHECK(in>=0&&out>=0);
    char bytes[16384];ssize_t got;
    while((got=read(in,bytes,sizeof(bytes)))>0)CHECK(write(out,bytes,(size_t)got)==got);
    CHECK(!got&&!fchmod(out,0755)&&!close(in)&&!close(out));
}
static void text_file(const char *path,const char *text)
{
    int fd=open(path,O_CREAT|O_TRUNC|O_WRONLY,0755);CHECK(fd>=0);
    size_t n=strlen(text);CHECK(write(fd,text,n)==(ssize_t)n&&!close(fd));
}
static void exec_ok(const char *path)
{
    pid_t child=fork();CHECK(child>=0);
    if(!child){char *argv[]={(char *)path,"--exec-ok",0};char *env[]={0};execve(path,argv,env);_exit(90);}
    joined(child,42);
}
static void exec_error(const char *path,int error)
{
    int report[2];CHECK(!pipe(report));pid_t child=fork();CHECK(child>=0);
    if(!child){
        CHECK(!close(report[0]));int saved=open("/ma/null",O_WRONLY|O_CLOEXEC);CHECK(saved>=0);
        char *argv[]={(char *)path,"--exec-ok",0};char *env[]={0};
        errno=0;int result=execve(path,argv,env),observed=errno;
        CHECK(result==-1&&(fcntl(saved,F_GETFD)&FD_CLOEXEC)&&write(saved,"k",1)==1);
        CHECK(write(report[1],&observed,sizeof(observed))==sizeof(observed));_exit(0);
    }
    CHECK(!close(report[1]));int observed=0;CHECK(read(report[0],&observed,sizeof(observed))==sizeof(observed));
    CHECK(!close(report[0]));joined(child,0);CHECK(observed==error);
}
static void options(const char *target,unsigned bits)
{
    struct statfs st;CHECK(!statfs(target,&st)&&((unsigned)st.f_flags&15U)==bits);
    int fd=open(target,O_PATH|O_DIRECTORY);CHECK(fd>=0&&!fstatfs(fd,&st)&&((unsigned)st.f_flags&15U)==bits&&!close(fd));
    FILE *f=fopen("/ma/proc/self/mounts","r");CHECK(f);char *line=0;size_t capacity=0;int found=0;
    while(getline(&line,&capacity,f)>0){
        char source[256],path[256],type[64],opts[256];
        if(sscanf(line,"%255s %255s %63s %255s",source,path,type,opts)!=4||strcmp(path,target))continue;
        unsigned actual=0;char *save=0;
        for(char *part=strtok_r(opts,",",&save);part;part=strtok_r(0,",",&save)){
            if(!strcmp(part,"ro"))actual|=1;
            if(!strcmp(part,"nosuid"))actual|=2;
            if(!strcmp(part,"nodev"))actual|=4;
            if(!strcmp(part,"noexec"))actual|=8;
        }
        CHECK(actual==bits);found++;
    }
    free(line);CHECK(!fclose(f)&&found==1);
}
static void setup(void)
{
    directory("/ma");CHECK(!mount("tmpfs","/ma","tmpfs",0,0));
    const char *dirs[]={"/ma/deny","/ma/proc","/ma/devpts","/ma/target","/ma/readonly","/ma/base","/ma/hold","/ma/peer"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++)directory(dirs[i]);
    CHECK(!mount("tmpfs","/ma/deny","tmpfs",MS_NOEXEC|MS_NODEV,0));
    CHECK(!mount("proc","/ma/proc","proc",MS_NOSUID|MS_NOEXEC|MS_NODEV,0));
    CHECK(!mount("devpts","/ma/devpts","devpts",MS_NOEXEC|MS_NODEV,"newinstance,ptmxmode=0666"));
    CHECK(!mount("tmpfs","/ma/readonly","tmpfs",MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV|MS_SILENT,0));
    options("/ma/deny",12);options("/ma/proc",14);options("/ma/devpts",12);options("/ma/readonly",15);
    CHECK(!umount("/ma/readonly"));
    CHECK(!mknod("/ma/null",S_IFCHR|0666,makedev(1,3)));
    copy_file("/ma/proc/self/exe","/ma/exe");copy_file("/ma/exe","/ma/deny/exe");
    pass("new-mount-flags-statfs-mounts");
}
static void execution(void)
{
    exec_ok("/ma/exe");exec_error("/ma/deny/exe",EACCES);
    int fd=open("/ma/deny/exe",O_RDONLY);char magic[4];CHECK(fd>=0&&read(fd,magic,4)==4&&!memcmp(magic,"\177ELF",4)&&!close(fd));
    text_file("/ma/deny/script","#!/ma/exe --exec-ok\n");exec_error("/ma/deny/script",EACCES);
    text_file("/ma/script","#!/ma/deny/exe --exec-ok\n");exec_error("/ma/script",EACCES);
    text_file("/ma/deny/bad","invalid executable\n");exec_error("/ma/deny/bad",EACCES);
    fd=open("/ma/deny/exe",O_WRONLY);CHECK(fd>=0);exec_error("/ma/deny/exe",EACCES);CHECK(!close(fd));
    pass("exec-script-format-writer-order");
}
/* 改一项非LOAD头即可到达解释器open；ENOENT对照证明没有在ELF格式检查处提前失败。 */
static void with_interpreter(const char *target,const char *interpreter)
{
    copy_file("/ma/exe",target);int fd=open(target,O_RDWR);CHECK(fd>=0);
    Elf64_Ehdr eh;CHECK(pread(fd,&eh,sizeof(eh),0)==sizeof(eh)&&eh.e_phentsize==sizeof(Elf64_Phdr));
    Elf64_Phdr ph,chosen={0};unsigned index=0;int found=0;
    for(unsigned i=0;i<eh.e_phnum;i++){
        CHECK(pread(fd,&ph,sizeof(ph),(off_t)eh.e_phoff+i*sizeof(ph))==sizeof(ph));
        if(ph.p_type==PT_INTERP){chosen=ph;index=i;found=1;break;}
        if(!found&&(ph.p_type==PT_GNU_STACK||ph.p_type==PT_NOTE)){chosen=ph;index=i;found=1;}
    }
    CHECK(found);off_t end=lseek(fd,0,SEEK_END);CHECK(end>0);
    size_t length=strlen(interpreter)+1;CHECK(write(fd,interpreter,length)==(ssize_t)length);
    chosen=(Elf64_Phdr){.p_type=PT_INTERP,.p_offset=(Elf64_Off)end,.p_filesz=length,.p_memsz=length,.p_align=1};
    CHECK(pwrite(fd,&chosen,sizeof(chosen),(off_t)eh.e_phoff+index*sizeof(chosen))==sizeof(chosen)&&!close(fd));
}
static void interpreter(void)
{
    with_interpreter("/ma/missing-interp","/ma/deny/missing");exec_error("/ma/missing-interp",ENOENT);
    with_interpreter("/ma/denied-interp","/ma/deny/exe");exec_error("/ma/denied-interp",EACCES);
    pass("elf-interpreter-final-mount");
}
static void links_and_children(void)
{
    CHECK(!symlink("/ma/deny/exe","/ma/into-deny")&&!symlink("/ma/exe","/ma/deny/outside"));
    exec_error("/ma/into-deny",EACCES);exec_ok("/ma/deny/outside");
    directory("/ma/deny/inner");CHECK(!mount("tmpfs","/ma/deny/inner","tmpfs",0,0));
    copy_file("/ma/exe","/ma/deny/inner/exe");exec_ok("/ma/deny/inner/exe");
    CHECK(!mknod("/ma/deny/inner/null",S_IFCHR|0666,makedev(1,3)));
    int fd=open("/ma/deny/inner/null",O_WRONLY);CHECK(fd>=0&&write(fd,"n",1)==1&&!close(fd));
    CHECK(!umount("/ma/deny/inner"));pass("symlinks-and-child-use-final-mount");
}
static void access_cases(void)
{
    CHECK(!access("/ma/deny",X_OK)&&!access("/ma/deny/exe",R_OK|W_OK));
    ERROR(access("/ma/deny/exe",X_OK),EACCES);CHECK(!access("/ma/deny/outside",X_OK));
    int fd=open("/ma/deny/exe",O_PATH);CHECK(fd>=0);
    ERROR(syscall(SYS_faccessat2,fd,"",X_OK,AT_EMPTY_PATH),EACCES);
    CHECK(!syscall(SYS_faccessat2,fd,"",R_OK,AT_EMPTY_PATH)&&!close(fd));
    pid_t child=fork();CHECK(child>=0);
    if(!child){CHECK(!setgid(1234)&&!setuid(1234));ERROR(access("/ma/deny/exe",X_OK),EACCES);
        ERROR(mount("tmpfs","/ma/target","tmpfs",MS_NOEXEC|MS_NODEV,0),EPERM);_exit(0);}
    joined(child,0);pass("access-directory-empty-fd-and-privilege");
}
static void device_cases(void)
{
    CHECK(!mknod("/ma/deny/null",S_IFCHR|0666,makedev(1,3)));
    CHECK(!mknod("/ma/deny/unknown",S_IFCHR|0666,makedev(240,99)));
    CHECK(!mknod("/ma/deny/block",S_IFBLK|0666,makedev(240,99)));
    const char *paths[]={"/ma/deny/null","/ma/deny/unknown","/ma/deny/block","/ma/devpts/ptmx"};
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++){
        ERROR(open(paths[i],O_RDWR|O_NONBLOCK),EACCES);int fd=open(paths[i],O_PATH);CHECK(fd>=0);
        struct stat st;CHECK(!fstat(fd,&st));char link[64];fdname(link,fd);
        ERROR(open(link,O_RDWR|O_NONBLOCK),EACCES);int path=open(link,O_PATH);CHECK(path>=0&&!close(path)&&!close(fd));
        ERROR(open(paths[i],O_PATH|O_DIRECTORY),ENOTDIR);
    }
    CHECK(!access("/ma/deny/null",R_OK|W_OK));
    CHECK(!symlink("/ma/deny/null","/ma/into-device")&&!symlink("/ma/null","/ma/deny/out-device"));
    ERROR(open("/ma/into-device",O_WRONLY),EACCES);ERROR(open("/ma/into-device",O_NOFOLLOW),ELOOP);
    int fd=open("/ma/deny/out-device",O_WRONLY);CHECK(fd>=0&&write(fd,"a",1)==1&&!close(fd));
    CHECK(!mkfifo("/ma/deny/fifo",0600));fd=open("/ma/deny/fifo",O_RDWR|O_NONBLOCK);CHECK(fd>=0&&!close(fd));
    pass("nodev-special-types-path-only-and-aliases");
}
static void mount_source(void)
{
    ERROR(mount("/ma/deny/block","/ma/target","ext4",0,0),EACCES);
    CHECK(!symlink("/ma/deny/block","/ma/block-link"));ERROR(mount("/ma/block-link","/ma/target","ext4",0,0),EACCES);
    int fd=open("/ma/deny/block",O_PATH);CHECK(fd>=0);char path[64];fdname(path,fd);
    ERROR(mount(path,"/ma/target","ext4",0,0),EACCES);CHECK(!close(fd));
    ERROR(mount("/ma/deny/exe","/ma/target","ext4",0,0),ENOTBLK);
    pass("nodev-block-mount-source-and-type-order");
}
static void mapping_cases(void)
{
    int fd=open("/ma/deny/data",O_CREAT|O_TRUNC|O_RDWR,0644);CHECK(fd>=0&&!ftruncate(fd,3*page));
    CHECK(pwrite(fd,"x",1,0)==1);
    for(unsigned shared=0;shared<2;shared++){
        int flag=shared?MAP_SHARED:MAP_PRIVATE;errno=0;
        CHECK(mmap(0,page,PROT_READ|PROT_EXEC,flag,fd,0)==MAP_FAILED&&errno==EPERM);
        unsigned char *p=mmap(0,3*page,PROT_READ,flag,fd,0);CHECK(p!=MAP_FAILED&&p[0]=='x');
        ERROR(mprotect(p,page,PROT_READ|PROT_EXEC),EACCES);
        CHECK(!mprotect(p+page,page,PROT_NONE));pid_t child=fork();CHECK(child>=0);
        if(!child){ERROR(mprotect(p+page,page,PROT_EXEC),EACCES);CHECK(p[0]=='x');_exit(0);}joined(child,0);
        void *destination=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(destination!=MAP_FAILED);
        CHECK(mremap(p+page,page,page,MREMAP_MAYMOVE|MREMAP_FIXED,destination)==destination);
        ERROR(mprotect(destination,page,PROT_EXEC),EACCES);
        CHECK(!munmap(destination,page)&&!munmap(p,page)&&!munmap(p+2*page,page));
    }
    unsigned char *p=mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);CHECK(p!=MAP_FAILED);p[0]='y';
    char byte;CHECK(pread(fd,&byte,1,0)==1&&byte=='x'&&!munmap(p,page));
    p=mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);p[0]=73;
    errno=0;CHECK(mmap(p,page,PROT_EXEC,MAP_FIXED|MAP_PRIVATE,fd,0)==MAP_FAILED&&errno==EPERM&&p[0]==73);
    errno=0;CHECK(mmap(p,page,PROT_EXEC,MAP_FIXED_NOREPLACE|MAP_PRIVATE,fd,0)==MAP_FAILED&&errno==EEXIST);
    CHECK(!munmap(p,page)&&!close(fd));
    text_file("/ma/data","x");CHECK(!chmod("/ma/data",0644));
    fd=open("/ma/data",O_RDONLY);CHECK(fd>=0);p=mmap(0,page,PROT_READ|PROT_EXEC,MAP_PRIVATE,fd,0);
    CHECK(p!=MAP_FAILED&&p[0]=='x'&&!munmap(p,page)&&!close(fd));
    p=mmap(0,page,PROT_READ|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED&&!munmap(p,page));
    pass("mmap-noexec-mprotect-fork-remap-and-fixed-content");
}
static void mapping_errors(void)
{
    int fd=open("/ma/deny/data",O_WRONLY),path=open("/ma/deny/data",O_PATH);CHECK(fd>=0&&path>=0);
    errno=0;CHECK(mmap(0,page,PROT_EXEC,MAP_PRIVATE,fd,0)==MAP_FAILED&&errno==EACCES);
    errno=0;CHECK(mmap(0,page,PROT_EXEC,MAP_PRIVATE,path,0)==MAP_FAILED&&errno==EBADF);
    errno=0;CHECK(mmap(0,page,PROT_EXEC,MAP_PRIVATE,-1,0)==MAP_FAILED&&errno==EBADF);
    CHECK(!close(fd)&&!close(path));fd=open("/ma/deny/data",O_RDONLY);CHECK(fd>=0);
    errno=0;CHECK(mmap(0,page,PROT_WRITE|PROT_EXEC,MAP_SHARED,fd,0)==MAP_FAILED&&errno==EACCES);
    errno=0;CHECK(mmap(0,0,PROT_EXEC,MAP_PRIVATE,fd,0)==MAP_FAILED&&errno==EINVAL);
    errno=0;CHECK(mmap(0,page,PROT_EXEC,MAP_PRIVATE,fd,1)==MAP_FAILED&&errno==EINVAL);CHECK(!close(fd));
    const char *paths[]={"/ma/proc/self/status","/ma/deny"};
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++){
        fd=open(paths[i],O_RDONLY);CHECK(fd>=0);errno=0;
        CHECK(mmap(0,page,PROT_EXEC,MAP_PRIVATE,fd,0)==MAP_FAILED&&errno==EPERM);errno=0;
        CHECK(mmap(0,page,PROT_READ,MAP_PRIVATE,fd,0)==MAP_FAILED&&errno==ENODEV);CHECK(!close(fd));
    }
    pass("mmap-fd-rights-validation-and-no-mmap-order");
}
static void old_fd_mounts(void)
{
    CHECK(!mount("tmpfs","/ma/base","tmpfs",0,0));copy_file("/ma/exe","/ma/base/exe");
    CHECK(!mknod("/ma/base/null",S_IFCHR|0666,makedev(1,3)));
    int exe=open("/ma/base/exe",O_PATH),device=open("/ma/base/null",O_WRONLY),dir=open("/ma/base",O_PATH|O_DIRECTORY);CHECK(exe>=0&&device>=0&&dir>=0);
    CHECK(!mount("tmpfs","/ma/base","tmpfs",MS_NOEXEC|MS_NODEV,0));copy_file("/ma/exe","/ma/base/exe");
    CHECK(!mknod("/ma/base/null",S_IFCHR|0666,makedev(1,3)));exec_error("/ma/base/exe",EACCES);ERROR(open("/ma/base/null",O_WRONLY),EACCES);
    char path[64];fdname(path,exe);exec_ok(path);fdname(path,device);int reopened=open(path,O_WRONLY);CHECK(reopened>=0);
    int duplicate=dup(device),relative=openat(dir,"null",O_WRONLY);CHECK(duplicate>=0&&relative>=0);
    CHECK(write(device,"x",1)==1&&write(duplicate,"x",1)==1&&write(relative,"x",1)==1&&write(reopened,"x",1)==1);
    struct statfs st;CHECK(!fstatfs(device,&st)&&!(st.f_flags&12));
    pid_t child=fork();CHECK(child>=0);if(!child){CHECK(write(device,"x",1)==1);_exit(0);}joined(child,0);
    int restricted=open("/ma/base/exe",O_RDONLY);CHECK(restricted>=0);
    CHECK(!mount("tmpfs","/ma/base","tmpfs",0,0));copy_file("/ma/exe","/ma/base/exe");exec_ok("/ma/base/exe");
    fdname(path,restricted);exec_error(path,EACCES);errno=0;
    CHECK(mmap(0,page,PROT_EXEC,MAP_PRIVATE,restricted,0)==MAP_FAILED&&errno==EPERM);
    CHECK(!close(restricted)&&!close(relative)&&!close(duplicate)&&!close(reopened)&&!close(device)&&!close(exe)&&!close(dir));
    CHECK(!umount("/ma/base")&&!umount("/ma/base")&&!umount("/ma/base"));
    pass("overmount-proc-dirfd-and-prior-fd-rights");
}
static void pty_peer(void)
{
    CHECK(!mount("devpts","/ma/peer","devpts",0,"newinstance,ptmxmode=0666"));
    int master=open("/ma/peer/ptmx",O_RDWR|O_NOCTTY);CHECK(master>=0);
    int locked=0;CHECK(!ioctl(master,TIOCSPTLCK,&locked));
    CHECK(!mount("devpts","/ma/peer","devpts",MS_NODEV,"newinstance,ptmxmode=0666"));
    ERROR(open("/ma/peer/ptmx",O_RDWR|O_NOCTTY),EACCES);
    int peer=ioctl(master,0x5441,O_RDWR|O_NOCTTY);CHECK(peer>=0);
    char bytes[2];CHECK(write(master,"p\n",2)==2&&read(peer,bytes,2)==2&&!memcmp(bytes,"p\n",2));
    struct statfs st;CHECK(!fstatfs(peer,&st)&&!(st.f_flags&MS_NODEV));
    CHECK(!close(peer)&&!close(master)&&!umount("/ma/peer")&&!umount("/ma/peer"));
    pass("prior-pty-master-peer-path-and-io");
}
static void mapping_lifetime(void)
{
    CHECK(!mount("tmpfs","/ma/hold","tmpfs",MS_NOEXEC,0));
    int fd=open("/ma/hold/data",O_CREAT|O_RDWR,0600);CHECK(fd>=0&&!ftruncate(fd,page));
    unsigned char *p=mmap(0,page,PROT_READ,MAP_PRIVATE,fd,0);CHECK(p!=MAP_FAILED&&!close(fd));
    int ready[2],release[2];CHECK(!pipe(ready)&&!pipe(release));pid_t child=fork();CHECK(child>=0);
    if(!child){CHECK(!p[0]);token(ready[1]);expect(release[0]);ERROR(mprotect(p,page,PROT_EXEC),EACCES);CHECK(!munmap(p,page));_exit(0);}
    expect(ready[0]);CHECK(!munmap(p,page));ERROR(umount("/ma/hold"),EBUSY);token(release[1]);joined(child,0);
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(release[0])&&!close(release[1])&&!umount("/ma/hold"));
    pass("mapping-owner-survives-close-and-fork-until-release");
}
int main(int argc,char **argv)
{
    if(argc>1&&!strcmp(argv[1],"--exec-ok"))return 42;
    setvbuf(stdout,0,_IONBF,0);page=(size_t)sysconf(_SC_PAGESIZE);CHECK(page==4096||page==16384);
    setup();execution();interpreter();links_and_children();access_cases();device_cases();mount_source();
    mapping_cases();mapping_errors();old_fd_mounts();pty_peer();mapping_lifetime();
    CHECK(!umount("/ma/devpts")&&!umount("/ma/proc")&&!umount("/ma/deny")&&!umount("/ma"));
    printf("MOUNT-ATTR END %u\n",cases);if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
