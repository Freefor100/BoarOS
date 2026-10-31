#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2,"CHROOT FAIL line=%d errno=%d %s\n",__LINE__,errno,#x); _exit(1); } } while (0)
#define ERROR(x,e) do { errno=0; CHECK((x)==-1 && errno==(e)); } while (0)
static unsigned cases;
static char self_interpreter[512];
static long change_root(const char *path) { return syscall(SYS_chroot,path); }
static long execute(int fd,const char *path,char *const args[],int flags)
{ char *env[]={0}; return syscall(SYS_execveat,fd,path,args,env,flags); }
static void directory(const char *path) { CHECK(!mkdir(path,0755) || errno==EEXIST); }
static void create(const char *path,const char *bytes)
{
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0755); CHECK(fd>=0);
    size_t n=strlen(bytes); CHECK(write(fd,bytes,n)==(ssize_t)n && !close(fd));
}
static void content_at(int dir,const char *path,const char *expected)
{
    char bytes[64]={0}; int fd=openat(dir,path,O_RDONLY); CHECK(fd>=0);
    CHECK(read(fd,bytes,sizeof(bytes)-1)==(ssize_t)strlen(expected) && !strcmp(bytes,expected) && !close(fd));
}
static void content(const char *path,const char *expected) { content_at(AT_FDCWD,path,expected); }
static void cwd(const char *expected)
{
    char bytes[512]; CHECK(getcwd(bytes,sizeof(bytes)) && !strcmp(bytes,expected));
    CHECK(syscall(SYS_getcwd,bytes,sizeof(bytes))==(long)strlen(expected)+1 && !strcmp(bytes,expected));
}
static void cwd_unreachable(const char *visible,char *global,size_t size)
{
    char bytes[512],libc_bytes[512]; long n=syscall(SYS_getcwd,bytes,sizeof(bytes));
    CHECK(n>0 && (size_t)n<=sizeof(bytes) && !bytes[n-1] && n==(long)strlen(bytes)+1 && !strncmp(bytes,"(unreachable)/",14));
    const char *path=bytes+13; size_t length=strlen(path),suffix=strlen(visible);
    /* A fixture may itself be chrooted below the namespace root. Outside
     * paths then include that hidden ancestor, while visible paths do not. */
    CHECK(!strcmp(visible,"/") || (length>=suffix && !strcmp(path+length-suffix,visible)));
    if (global) { CHECK(length<size); memcpy(global,path,length+1); }
    ERROR(syscall(SYS_getcwd,libc_bytes,4),ERANGE);
    errno=0; CHECK(!getcwd(libc_bytes,sizeof(libc_bytes)) && errno==ENOENT);
}
static void same(const char *a,const char *b)
{ struct stat x,y; CHECK(!stat(a,&x) && !stat(b,&y) && x.st_dev==y.st_dev && x.st_ino==y.st_ino); }
static void same_cwd(const struct stat *before)
{ struct stat after; CHECK(!stat(".",&after) && before->st_dev==after.st_dev && before->st_ino==after.st_ino); }
static void link_text(const char *path,char *bytes,size_t size)
{ ssize_t n=readlink(path,bytes,size-1); CHECK(n>=0 && (size_t)n<size-1); bytes[n]=0; }
static void link_is(const char *path,const char *expected)
{
    char bytes[512]; link_text(path,bytes,sizeof(bytes));
    if (strcmp(bytes,expected)) dprintf(2,"CHROOT FAIL readlink path=%s actual=%s expected=%s\n",path,bytes,expected);
    CHECK(!strcmp(bytes,expected));
}
static int line_path(const char *line,const char *path)
{
    size_t n=strcspn(line,"\n"),length=strlen(path);
    return n>length && (line[n-length-1]==' ' || line[n-length-1]=='\t') && !strncmp(line+n-length,path,length);
}
static void mapped_paths(const char *exe,const char *outside)
{
    FILE *maps=fopen("/proc/self/maps","r"); CHECK(maps); char line[1024]; int found_exe=0,found_outside=0;
    while (fgets(line,sizeof(line),maps)) { found_exe|=line_path(line,exe); found_outside|=line_path(line,outside); }
    CHECK(!ferror(maps) && !fclose(maps) && found_exe && found_outside);
}
static void token(int fd,char c) { CHECK(write(fd,&c,1)==1); }
static void expect(int fd,char c) { char b; CHECK(read(fd,&b,1)==1 && b==c); }
static void joined(pid_t child,int code)
{ int status; CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==code); }
static void run(const char *name,void (*test)(void))
{
    pid_t child=fork(); CHECK(child>=0); if (!child) { test(); _exit(0); }
    joined(child,0); printf("CHROOT CASE %s PASS\n",name); cases++;
}
static void copy(const char *from,const char *to)
{
    int in=open(from,O_RDONLY),out=open(to,O_WRONLY|O_CREAT|O_TRUNC,0755); CHECK(in>=0 && out>=0);
    char bytes[8192]; ssize_t n;
    while ((n=read(in,bytes,sizeof(bytes)))>0) CHECK(write(out,bytes,(size_t)n)==n);
    CHECK(!n && !fchmod(out,0755) && !close(in) && !close(out));
}
static void jail_path(char *out,size_t size,const char *jail,const char *path)
{ CHECK(snprintf(out,size,"%s%s",jail,path)>0 && strlen(jail)+strlen(path)<size); }
static void copy_into(const char *jail,const char *from)
{
    char path[1024]; jail_path(path,sizeof(path),jail,from);
    for (char *p=path+1;*p;p++) if (*p=='/') { *p=0; directory(path); *p='/'; }
    copy(from,path);
}
static void interpreter(const char *file,char *path,size_t size)
{
    int fd=open(file,O_RDONLY); Elf64_Ehdr header; CHECK(fd>=0 && pread(fd,&header,sizeof(header),0)==sizeof(header));
    CHECK(!memcmp(header.e_ident,ELFMAG,SELFMAG) && header.e_ident[EI_CLASS]==ELFCLASS64 && header.e_phentsize==sizeof(Elf64_Phdr));
    path[0]=0;
    for (unsigned i=0;i<header.e_phnum;i++) {
        Elf64_Phdr p; CHECK(pread(fd,&p,sizeof(p),(off_t)header.e_phoff+i*sizeof(p))==sizeof(p));
        if (p.p_type==PT_INTERP) {
            CHECK(p.p_filesz>1 && p.p_filesz<size && pread(fd,path,(size_t)p.p_filesz,(off_t)p.p_offset)==(ssize_t)p.p_filesz);
            CHECK(path[0]=='/' && !path[p.p_filesz-1]);
        }
    }
    CHECK(!close(fd));
}
static void install_runtime(const char *jail,const char *file,char *path,size_t size)
{
    interpreter(file,path,size); if (path[0]) copy_into(jail,path);
    /* The reference BusyBox may name musl's loader and libc separately. */
    if (path[0] && access("/lib/libc.so",R_OK)==0 && strcmp(path,"/lib/libc.so")) copy_into(jail,"/lib/libc.so");
}
static void setup(void)
{
    directory("/cr"); CHECK(!mount("tmpfs","/cr","tmpfs",0,0));
    const char *dirs[]={"/cr/jail","/cr/jail/sub","/cr/jail/sub/nested","/cr/outside","/cr/no-search","/cr/no-search/inside","/cr/no-loader","/cr/busy","/cr/busy/bin","/cr/busy/sub","/cr/rename","/cr/bind-src","/cr/bind-view","/cr/detach"};
    for (unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++) directory(dirs[i]);
    create("/cr/file","file"); create("/cr/jail/token","inside\n"); create("/cr/jail/sub/token","sub\n");
    create("/cr/outside/held","outside\n"); create("/cr/no-loader/token","no-loader\n");
    create("/cr/busy/token","busy\n"); create("/cr/rename/token","rename\n");
    CHECK(!chmod("/cr/no-search",0000));
    CHECK(!symlink("jail","/cr/entry") && !symlink("loop","/cr/loop"));
    CHECK(!symlink("/token","/cr/jail/absolute") && !symlink("../token","/cr/jail/sub/up"));
    CHECK(!symlink("/../../token","/cr/jail/slash-dotdot") && !symlink("../../outside/held","/cr/jail/escape"));
    copy("/init","/cr/jail/exec"); install_runtime("/cr/jail","/init",self_interpreter,sizeof(self_interpreter));
    copy("/busybox","/cr/busy/bin/busybox"); char bb_interpreter[512];
    install_runtime("/cr/busy","/busybox",bb_interpreter,sizeof(bb_interpreter));
}
static void errors(void)
{
    ERROR(change_root((char *)(uintptr_t)1),EFAULT); ERROR(change_root(""),ENOENT);
    ERROR(change_root("/cr/missing"),ENOENT); ERROR(change_root("/cr/file"),ENOTDIR);
    ERROR(change_root("/cr/file/.."),ENOTDIR); ERROR(change_root("/cr/loop"),ELOOP);
    char long_name[4097]; memset(long_name,'x',sizeof(long_name)); long_name[sizeof(long_name)-1]=0;
    ERROR(change_root(long_name),ENAMETOOLONG);
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        CHECK(!syscall(SYS_setresgid,20161,20161,20161) && !syscall(SYS_setresuid,20161,20161,20161));
        ERROR(change_root((char *)(uintptr_t)1),EFAULT); ERROR(change_root("/cr/missing"),ENOENT);
        ERROR(change_root("/cr/file"),ENOTDIR); ERROR(change_root("/cr/no-search"),EACCES);
        ERROR(change_root("/cr/no-search/inside"),EACCES); ERROR(change_root("/cr/loop"),ELOOP);
        ERROR(change_root("/cr/jail"),EPERM); content("/cr/jail/token","inside\n"); _exit(0);
    }
    joined(child,0); child=fork(); CHECK(child>=0);
    if (!child) {
        struct { uint32_t version; int32_t pid; } header={0x20080522,0};
        struct { uint32_t effective,permitted,inheritable; } caps[2];
        CHECK(!syscall(SYS_capget,&header,caps)); caps[0].effective&=~(UINT32_C(1)<<18);
        CHECK(!syscall(SYS_capset,&header,caps)); ERROR(change_root("/cr/jail"),EPERM); _exit(0);
    }
    joined(child,0); cwd("/");
}
static void root_cwd(void)
{
    directory("/cr/jail/proc"); CHECK(!mount("proc","/cr/jail/proc","proc",0,0));
    int outside=open("/cr/outside/held",O_RDONLY); CHECK(outside>=0); size_t page=(size_t)sysconf(_SC_PAGESIZE); CHECK(page>0);
    const char *mapping=mmap(0,page,PROT_READ,MAP_PRIVATE,outside,0); CHECK(mapping!=MAP_FAILED);
    char exe[512],file[512],old_cwd[512],global_cwd[512],global_exe[1024],global_file[1024],fdpath[64];
    CHECK(snprintf(fdpath,sizeof(fdpath),"/cr/jail/proc/self/fd/%d",outside)>0);
    link_text("/cr/jail/proc/self/exe",exe,sizeof(exe)); link_text(fdpath,file,sizeof(file));
    CHECK(!chdir("/cr/outside")); struct stat before; CHECK(!stat(".",&before));
    link_text("/cr/jail/proc/self/cwd",old_cwd,sizeof(old_cwd));
    CHECK(!change_root("/cr/jail"));
    cwd_unreachable(old_cwd,global_cwd,sizeof(global_cwd)); same_cwd(&before);
    int prefix=(int)(strlen(global_cwd)-strlen(old_cwd));
    CHECK(snprintf(global_exe,sizeof(global_exe),"%.*s%s",prefix,global_cwd,exe)>0 &&
          snprintf(global_file,sizeof(global_file),"%.*s%s",prefix,global_cwd,file)>0);
    link_is("/proc/self/root","/"); link_is("/proc/self/cwd",global_cwd); link_is("/proc/self/exe",global_exe);
    CHECK(snprintf(fdpath,sizeof(fdpath),"/proc/self/fd/%d",outside)>0); link_is(fdpath,global_file);
    mapped_paths(global_exe,global_file); CHECK(!memcmp(mapping,"outside\n",8) && !munmap((void *)mapping,page) && !close(outside));
    content("held","outside\n"); content("/token","inside\n");
    CHECK(!chdir("..")); content("outside/held","outside\n");
    CHECK(!chdir("/")); cwd("/"); same("/","/../../..");
    /* A chroot subdirectory is not the root dentry of its containing mount. */
    ERROR(umount("/"),EINVAL); ERROR(umount2("/",MNT_DETACH),EINVAL); content("/token","inside\n");
    CHECK(!umount("/proc"));
}
static void paths(void)
{
    CHECK(!change_root("/cr/entry") && !chdir("/"));
    content("/absolute","inside\n"); content("/slash-dotdot","inside\n");
    CHECK(!chdir("sub/nested")); content("../../token","inside\n"); content("../up","inside\n");
    CHECK(!chdir("/sub/../../..")); cwd("/"); same("/","/../..");
    ERROR(open("/escape",O_RDONLY),ENOENT); ERROR(open("/cr/outside/held",O_RDONLY),ENOENT);
    CHECK(!change_root("sub") && !chdir("/")); cwd("/"); content("/token","sub\n");
}
static void held(void)
{
    int outside=open("/cr/outside",O_PATH|O_DIRECTORY),file=open("/cr/outside/held",O_RDWR); CHECK(outside>=0 && file>=0);
    CHECK(!change_root("/cr/jail") && !chdir("/")); int root=open("/",O_PATH|O_DIRECTORY); CHECK(root>=0);
    content_at(outside,"held","outside\n"); content_at(outside,"../jail/token","inside\n"); content_at(outside,"/token","inside\n");
    char bytes[8]; CHECK(pread(file,bytes,sizeof(bytes),0)==sizeof(bytes) && !memcmp(bytes,"outside\n",sizeof(bytes)));
    CHECK(pwrite(file,"outside\n",8,0)==8 && !fchdir(outside)); cwd_unreachable("/cr/outside",0,0);
    content("held","outside\n"); content("/token","inside\n");
    CHECK(!fchdir(root) && !close(outside) && !close(file) && !close(root)); cwd("/");
}
static void fork_copy(void)
{
    CHECK(!change_root("/cr/jail") && !chdir("/")); int to[2],from[2]; CHECK(!pipe(to) && !pipe(from));
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        CHECK(!close(to[1]) && !close(from[0]));
        cwd("/"); content("/token","inside\n"); CHECK(!change_root("sub") && !chdir("/"));
        token(from[1],'r'); expect(to[0],'x'); content("/token","sub\n"); _exit(0);
    }
    CHECK(!close(to[0]) && !close(from[1]));
    expect(from[0],'r'); cwd("/"); content("/token","inside\n"); token(to[1],'x'); joined(child,0);
    CHECK(!close(to[1]) && !close(from[0]));
}
struct gate { int ready,go,unused_ready,unused_go; };
static void *thread_root(void *opaque)
{
    struct gate *g=opaque; CHECK(!change_root("/cr/jail")); token(g->ready,'r'); expect(g->go,'x');
    cwd("/"); content("/token","inside\n"); return 0;
}
static void pthread_shared(void)
{
    int to[2],from[2]; CHECK(!pipe(to) && !pipe(from)); struct gate gate={.ready=from[1],.go=to[0]}; pthread_t thread;
    struct stat before; CHECK(!stat(".",&before));
    CHECK(!pthread_create(&thread,0,thread_root,&gate)); expect(from[0],'r'); content("/token","inside\n");
    cwd_unreachable("/",0,0); same_cwd(&before); CHECK(!chdir("/")); token(to[1],'x'); CHECK(!pthread_join(thread,0));
    CHECK(!close(to[0]) && !close(to[1]) && !close(from[0]) && !close(from[1]));
}
static int clone_root(void *opaque)
{ struct gate *g=opaque; CHECK(!close(g->unused_ready) && !close(g->unused_go)); thread_root(g); return 0; }
static void clone_shared(void)
{
    int to[2],from[2]; CHECK(!pipe(to) && !pipe(from)); struct gate gate={from[1],to[0],from[0],to[1]};
    struct stat before; CHECK(!stat(".",&before));
    char *stack=malloc(65536); CHECK(stack); pid_t child=clone(clone_root,stack+65536,CLONE_FS|SIGCHLD,&gate); CHECK(child>=0);
    CHECK(!close(to[0]) && !close(from[1]));
    expect(from[0],'r'); content("/token","inside\n"); cwd_unreachable("/",0,0); same_cwd(&before);
    CHECK(!chdir("/")); token(to[1],'x'); joined(child,0); free(stack);
    CHECK(!close(to[1]) && !close(from[0]));
}
static void renamed(void)
{
    int to[2],from[2]; CHECK(!pipe(to) && !pipe(from)); pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        CHECK(!close(to[1]) && !close(from[0]));
        CHECK(!change_root("/cr/rename") && !chdir("/")); struct stat before,after; CHECK(!stat("/",&before));
        token(from[1],'r'); expect(to[0],'x'); content("/token","rename\n"); cwd("/"); same("/","/..");
        CHECK(!stat("/",&after) && before.st_dev==after.st_dev && before.st_ino==after.st_ino); _exit(0);
    }
    CHECK(!close(to[0]) && !close(from[1]));
    expect(from[0],'r'); CHECK(!rename("/cr/rename","/cr/renamed")); directory("/cr/rename"); create("/cr/rename/token","replacement\n");
    token(to[1],'x'); joined(child,0); content("/cr/renamed/token","rename\n");
    CHECK(!close(to[1]) && !close(from[0]));
}
static void bound(void)
{
    CHECK(!mount("tmpfs","/cr/bind-src","tmpfs",0,0)); directory("/cr/bind-src/sub");
    create("/cr/bind-src/token","parent\n"); create("/cr/bind-src/sub/token","bound\n");
    CHECK(!mount("/cr/bind-src/sub","/cr/bind-view",0,MS_BIND,0)); pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        CHECK(!change_root("/cr/bind-view") && !chdir("/")); cwd("/"); same("/","/../.."); content("/../token","bound\n");
        CHECK(!umount("/")); ERROR(open("/write",O_WRONLY|O_CREAT,0600),EROFS); content("/token","bound\n"); _exit(0);
    }
    joined(child,0); CHECK(!umount2("/cr/bind-view",MNT_DETACH) && !umount("/cr/bind-src"));
}
static void detached(void)
{
    CHECK(!mount("tmpfs","/cr/detach","tmpfs",0,0)); create("/cr/detach/token","detached\n"); directory("/cr/detach/sub");
    int to[2],from[2]; CHECK(!pipe(to) && !pipe(from)); pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        CHECK(!close(to[1]) && !close(from[0]));
        /* Keep cwd outside so the fs root is the only child path owner at detach. */
        struct stat before; CHECK(!stat(".",&before));
        CHECK(!change_root("/cr/detach")); token(from[1],'r'); expect(to[0],'x');
        content("/token","detached\n"); cwd_unreachable("/",0,0); same_cwd(&before); CHECK(!chdir("/sub")); cwd("/sub");
        CHECK(!chdir("../..")); cwd("/"); content("/token","detached\n"); _exit(0);
    }
    CHECK(!close(to[0]) && !close(from[1]));
    expect(from[0],'r'); CHECK(!umount2("/cr/detach",MNT_DETACH)); ERROR(open("/cr/detach/token",O_RDONLY),ENOENT);
    CHECK(!mount("tmpfs","/cr/detach","tmpfs",0,0)); create("/cr/detach/token","replacement\n");
    token(to[1],'x'); joined(child,0); content("/cr/detach/token","replacement\n"); CHECK(!umount("/cr/detach"));
    CHECK(!close(to[1]) && !close(from[0]));
}
static void failed_exec(void)
{
    CHECK(!change_root("/cr/jail") && !chdir("/")); int keep=open("/token",O_RDONLY|O_CLOEXEC); CHECK(keep>=0);
    char *args[]={"failed",0}; ERROR(execute(AT_FDCWD,"/missing",args,0),ENOENT);
    ERROR(execute(AT_FDCWD,"/token",args,0),ENOEXEC); ERROR(execute(AT_FDCWD,"/",args,0),EACCES);
    CHECK(fcntl(keep,F_GETFD)==FD_CLOEXEC && !close(keep)); cwd("/"); content("/token","inside\n");
}
static void rooted_exec(void)
{
    int fd=open("/init",O_PATH|O_CLOEXEC); CHECK(fd>=0); char *args[]={"rooted","--inside",0};
    for (unsigned i=0;i<3;i++) {
        pid_t child=fork(); CHECK(child>=0);
        if (!child) {
            CHECK(!change_root("/cr/jail") && !chdir("/"));
            ERROR(open("/proc/self/fd",O_RDONLY|O_DIRECTORY),ENOENT);
            if (i==2) { char *env[]={0}; fexecve(fd,args,env); }
            else execute(i?AT_FDCWD:fd,i?"/exec":"",args,i?0:AT_EMPTY_PATH);
            CHECK(0);
        }
        joined(child,42);
    }
    if (self_interpreter[0]) {
        pid_t child=fork(); CHECK(child>=0);
        if (!child) {
            CHECK(!change_root("/cr/no-loader") && !chdir("/")); ERROR(execute(fd,"",args,AT_EMPTY_PATH),ENOENT);
            content("/token","no-loader\n"); cwd("/"); _exit(0);
        }
        joined(child,0);
    }
    CHECK(!close(fd));
}
static void busybox(void)
{
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        char *args[]={"busybox","chroot","/cr/busy","/bin/busybox","sh","-c",
            "test \"$(/bin/busybox cat /token)\" = busy && test \"$PWD\" = / && test ! -e /outside && cd /sub/../../.. && test \"$PWD\" = / && /bin/busybox test -f /token",0};
        execv("/busybox",args); CHECK(0);
    }
    joined(child,0);
}
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0);
    if (argc==2 && !strcmp(argv[1],"--inside")) {
        content("/token","inside\n"); cwd("/"); ERROR(open("/cr/outside/held",O_RDONLY),ENOENT); return 42;
    }
    /* Keep the missing-syscall probe first for the fixed-kernel RED baseline. */
    ERROR(change_root((void *)0),EFAULT);
    CHECK(!chdir("/")); setup();
    run("pointer-path-type-and-capability-order",errors);
    run("root-cwd-separation-and-unreachable",root_cwd);
    run("absolute-relative-symlink-and-dotdot",paths);
    run("held-outside-dirfd-and-fchdir",held);
    run("fork-root-copy-and-inheritance",fork_copy);
    run("pthread-shared-root",pthread_shared);
    run("clone-fs-shared-root",clone_shared);
    run("renamed-root-identity",renamed);
    run("bind-root-mount-identity-and-parent",bound);
    run("detached-root-last-owner",detached);
    run("failed-exec-preserves-root",failed_exec);
    run("execveat-held-outside-elf-and-rooted-interpreter",rooted_exec);
    run("busybox-chroot-exec-and-rooted-paths",busybox);
    CHECK(!umount("/cr")); printf("CHROOT END %u\n",cases);
    if (getpid()==1) { sync(); reboot(RB_POWER_OFF); }
    return 0;
}
