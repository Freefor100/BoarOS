#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2,"MOUNTNS FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1); } } while (0)
#define ERROR(x,e) do { errno=0;CHECK((x)==-1 && errno==(e)); } while (0)
static const char data_path[]="/ns-data";
static const char nested_path[]="/ns-data/nested";
static const char marker_path[]="/ns-data/nested/marker";
static const char private_path[]="/ns-data/private";
static const char hidden_path[]="/ns-data/private/hidden";
static const char self_ns[]="/ns-proc/self/ns/mnt";
static char clone_stack[65536] __attribute__((aligned(16)));

static void touch(const char *name)
{
    int fd=open(name,O_CREAT|O_WRONLY|O_EXCL,0644);
    CHECK(fd>=0 && write(fd,"x",1)==1 && !close(fd));
}

static void marker(int dirfd,const char *name)
{
    int fd=openat(dirfd,name,O_RDONLY);
    char data;
    if(fd<0)dprintf(2,"MOUNTNS marker path=%s errno=%d\n",name,errno);
    CHECK(fd>=0 && read(fd,&data,1)==1 && data=='x' && !close(fd));
}

static int mount_listing(pid_t pid,const char *mountpoint)
{
    char path[64],text[4096];
    CHECK(snprintf(path,sizeof(path),"/ns-proc/%d/mounts",(int)pid)>0);
    int fd=open(path,O_RDONLY);CHECK(fd>=0);
    ssize_t got=read(fd,text,sizeof(text)-1);
    CHECK(got>=0 && !close(fd));
    text[got]=0;
    return strstr(text,mountpoint)!=NULL;
}

static void joined(pid_t child)
{
    int status=0;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
}

static int clone_isolate(void *unused)
{
    (void)unused;
    marker(AT_FDCWD,marker_path);
    CHECK(!umount(nested_path));
    ERROR(access(marker_path,F_OK),ENOENT);
    return 0;
}

static int clone_share(void *arg)
{
    int fd=*(int *)arg;
    ERROR(setns(fd,CLONE_NEWNS),EINVAL);
    return 0;
}

struct mount_row {unsigned long id,parent,shared,master;int unbindable;unsigned major,minor;char root[256];};
static int mount_row(const char *path,struct mount_row *out)
{
    FILE *f=fopen("/mp/proc/self/mountinfo","r");CHECK(f);char *line=NULL;size_t capacity=0;int found=0;
    while(getline(&line,&capacity,f)>0){
        unsigned long id,parent;char device[64],root[256],point[256],options[256];int end=0;
        if(sscanf(line,"%lu %lu %63s %255s %255s %255s %n",&id,&parent,device,root,point,options,&end)!=6)continue;
        if(strcmp(point,path))continue;
        *out=(struct mount_row){.id=id,.parent=parent};strcpy(out->root,root);
        CHECK(sscanf(device,"%u:%u",&out->major,&out->minor)==2);
        char *limit=strstr(line+end," - ");if(!limit&&line[end]=='-')limit=line+end;CHECK(limit);*limit=0;
        char *value=strstr(line+end,"shared:");if(value)out->shared=strtoul(value+7,NULL,10);
        value=strstr(line+end,"master:");if(value)out->master=strtoul(value+7,NULL,10);
        out->unbindable=strstr(line+end,"unbindable")!=NULL;found++;
    }
    free(line);CHECK(!fclose(f)&&found<=1);return found;
}
static void make_directory(const char *name){CHECK(!mkdir(name,0755));}
static void propagation_edges(void)
{
    const char *sets[]={"/mp/w","/mp/l1","/mp/l2","/mp/repl","/mp/m1","/mp/m2"};
    char a[64],b[64],c[64],d[64],x[80],y[96],z[112];
    for(unsigned n=0;n<6;n++) {
        make_directory(sets[n]);CHECK(!mount("tmpfs",sets[n],"tmpfs",0,NULL));CHECK(snprintf(a,sizeof(a),"%s/a",sets[n])>0);
        CHECK(snprintf(b,sizeof(b),"%s/b",sets[n])>0);make_directory(a);make_directory(b);
        CHECK(!mount("tmpfs",a,"tmpfs",0,NULL));CHECK(snprintf(x,sizeof(x),"%s/x",a)>0);make_directory(x);
        CHECK(!mount(NULL,a,NULL,MS_SHARED,NULL)&&!mount(a,b,NULL,MS_BIND,NULL));
        if(n==0) {
            CHECK(!mount(NULL,b,NULL,MS_SLAVE,NULL)&&!mount(NULL,b,NULL,MS_SHARED,NULL));
            CHECK(snprintf(c,sizeof(c),"%s/c",sets[n])>0&&snprintf(d,sizeof(d),"%s/d",sets[n])>0);make_directory(c);make_directory(d);
            CHECK(!mount(b,c,NULL,MS_BIND,NULL)&&!mount(NULL,c,NULL,MS_SLAVE,NULL));
            CHECK(snprintf(y,sizeof(y),"%s/sub",b)>0);make_directory(y);
            CHECK(!mount(y,d,NULL,MS_BIND,NULL)&&!umount(b));
            CHECK(!mount("tmpfs",x,"tmpfs",0,NULL));CHECK(snprintf(y,sizeof(y),"%s/marker",x)>0);touch(y);
            CHECK(snprintf(y,sizeof(y),"%s/x/marker",c)>0);marker(AT_FDCWD,y);
        } else if(n==1) {
            CHECK(!mount("tmpfs",x,"tmpfs",0,NULL));CHECK(snprintf(y,sizeof(y),"%s/marker",x)>0);touch(y);
            CHECK(!umount2(a,MNT_DETACH));CHECK(snprintf(y,sizeof(y),"%s/x/marker",b)>0);ERROR(access(y,F_OK),ENOENT);
        } else if(n==2) {
            CHECK(!mount(NULL,b,NULL,MS_SLAVE,NULL)&&!mount("tmpfs",x,"tmpfs",0,NULL));
            CHECK(snprintf(y,sizeof(y),"%s/x",b)>0);CHECK(!mount(NULL,y,NULL,MS_PRIVATE,NULL));
            CHECK(snprintf(z,sizeof(z),"%s/local",y)>0);make_directory(z);CHECK(!mount("tmpfs",z,"tmpfs",0,NULL));
            CHECK(snprintf(y,sizeof(y),"%s/marker",z)>0);touch(y);
            CHECK(!umount2(x,MNT_DETACH));marker(AT_FDCWD,y);
        } else if(n==3) {
            CHECK(!mount(NULL,b,NULL,MS_SLAVE,NULL)&&!mount("tmpfs",x,"tmpfs",0,NULL));
            CHECK(snprintf(y,sizeof(y),"%s/x",b)>0);CHECK(!umount(y)&&!mount("tmpfs",y,"tmpfs",0,NULL));
            CHECK(snprintf(z,sizeof(z),"%s/held",y)>0);touch(z);int fd=open(z,O_RDONLY);CHECK(fd>=0);
            ERROR(umount(x),EBUSY);CHECK(!close(fd)&&!umount(x));ERROR(access(z,F_OK),ENOENT);
        } else if(n==4) {
            CHECK(snprintf(y,sizeof(y),"%s/x",b)>0);
            CHECK(!mount(a,y,NULL,MS_MOVE,NULL));CHECK(snprintf(z,sizeof(z),"%s/x",y)>0);
            struct mount_row row;CHECK(mount_row(z,&row));
        } else {
            CHECK(!mount(NULL,b,NULL,MS_SLAVE,NULL));CHECK(snprintf(y,sizeof(y),"%s/x",b)>0);
            CHECK(!mount("tmpfs",y,"tmpfs",0,NULL));CHECK(snprintf(z,sizeof(z),"%s/live",y)>0);touch(z);
            CHECK(!mount(y,x,NULL,MS_MOVE,NULL));CHECK(snprintf(z,sizeof(z),"%s/live",x)>0);marker(AT_FDCWD,z);
            CHECK(snprintf(z,sizeof(z),"%s/live",y)>0);marker(AT_FDCWD,z);
        }
        CHECK(!mount(NULL,sets[n],NULL,MS_PRIVATE|MS_REC,NULL)&&!umount2(sets[n],MNT_DETACH));
    }
    puts("PROP CASE narrow-master-lazy-trimming-replacement-and-move-aliases PASS");
    struct mount_row row;struct stat info;CHECK(!stat("/",&info)&&mount_row("/",&row)&&makedev(row.major,row.minor)==info.st_dev);
    make_directory("/mp/jail");CHECK(!mount("tmpfs","/mp/jail","tmpfs",0,NULL));make_directory("/mp/jail/proc");
    CHECK(!mount("proc","/mp/jail/proc","proc",0,NULL));pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!chroot("/mp/jail")&&!chdir("/"));FILE *f=fopen("/proc/self/mountinfo","r");CHECK(f);
        char *line=NULL;size_t size=0;int found=0;
        while(getline(&line,&size,f)>0){char root[256],point[256];unsigned long id,parent;unsigned major,minor;
            if(sscanf(line,"%lu %lu %u:%u %255s %255s",&id,&parent,&major,&minor,root,point)==6&&!strcmp(point,"/"))found++;
        }
        CHECK(found==1&&!fclose(f));free(line);_exit(0);
    }
    joined(child);CHECK(!umount("/mp/jail/proc")&&!umount("/mp/jail"));
    puts("PROP CASE mountinfo-device-and-chroot-root PASS");
}
static int propagation(void)
{
    setvbuf(stdout,NULL,_IONBF,0);make_directory("/mp");CHECK(!mount("tmpfs","/mp","tmpfs",0,NULL));
    const char *names[]={"/mp/a","/mp/b","/mp/c","/mp/d","/mp/u","/mp/r","/mp/proc"};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)make_directory(names[i]);
    CHECK(!mount("proc","/mp/proc","proc",0,NULL)&&!mount("tmpfs","/mp/a","tmpfs",0,NULL));
    const char *children[]={"x","y","z","t","nschild","post"};char path[128];
    for(unsigned i=0;i<sizeof(children)/sizeof(children[0]);i++){CHECK(snprintf(path,sizeof(path),"/mp/a/%s",children[i])>0);make_directory(path);}
    CHECK(!mount(NULL,"/mp/a",NULL,MS_SHARED,NULL)&&!mount("/mp/a","/mp/b",NULL,MS_BIND,NULL));
    struct mount_row a,b,c,d;CHECK(mount_row("/mp/a",&a)&&mount_row("/mp/b",&b)&&a.shared&&a.shared==b.shared&&a.id!=b.id);
    CHECK(!mount("tmpfs","/mp/a/x","tmpfs",0,NULL));touch("/mp/a/x/marker");marker(AT_FDCWD,"/mp/b/x/marker");
    CHECK(mount_row("/mp/a/x",&c)&&mount_row("/mp/b/x",&d)&&c.shared&&c.shared==d.shared);
    CHECK(!umount("/mp/a/x"));CHECK(!mount_row("/mp/a/x",&c)&&!mount_row("/mp/b/x",&d));
    puts("PROP CASE shared-peers-mountinfo-and-unmount PASS");
    CHECK(!mount("/mp/a","/mp/c",NULL,MS_BIND,NULL)&&!mount(NULL,"/mp/c",NULL,MS_SLAVE,NULL));
    CHECK(mount_row("/mp/c",&c)&&c.master==a.shared&&!c.shared);
    CHECK(!mount("tmpfs","/mp/c/z","tmpfs",0,NULL));touch("/mp/c/z/only");ERROR(access("/mp/a/z/only",F_OK),ENOENT);
    CHECK(!mount(NULL,"/mp/c",NULL,MS_SHARED,NULL)&&!mount("/mp/c","/mp/d",NULL,MS_BIND,NULL));
    CHECK(mount_row("/mp/c",&c)&&mount_row("/mp/d",&d)&&c.shared&&c.shared==d.shared&&c.master==a.shared&&d.master==a.shared);
    CHECK(!mount("tmpfs","/mp/a/t","tmpfs",0,NULL));touch("/mp/a/t/marker");
    marker(AT_FDCWD,"/mp/b/t/marker");marker(AT_FDCWD,"/mp/c/t/marker");marker(AT_FDCWD,"/mp/d/t/marker");
    CHECK(mount_row("/mp/a/t",&a)&&mount_row("/mp/c/t",&c)&&a.shared&&c.master==a.shared&&c.shared!=a.shared);
    CHECK(!umount("/mp/a/t"));CHECK(!mount_row("/mp/c/t",&c)&&!mount_row("/mp/d/t",&d));
    puts("PROP CASE slave-direction-shared-slave-and-descendants PASS");
    /* A pre-existing private receiver mount must remain below the incoming
     * event, without changing its logical downstream propagation position. */
    CHECK(!mount("tmpfs","/mp/a/z","tmpfs",0,NULL));touch("/mp/a/z/incoming");
    ERROR(access("/mp/c/z/incoming",F_OK),ENOENT);marker(AT_FDCWD,"/mp/c/z/only");
    marker(AT_FDCWD,"/mp/d/z/incoming");
    CHECK(!umount("/mp/d/z"));ERROR(access("/mp/d/z/incoming",F_OK),ENOENT);
    marker(AT_FDCWD,"/mp/a/z/incoming");marker(AT_FDCWD,"/mp/c/z/only");CHECK(!umount("/mp/a/z"));
    CHECK(!mount("tmpfs","/mp/a/x","tmpfs",0,NULL));
    CHECK(!mount(NULL,"/mp/b/x",NULL,MS_PRIVATE,NULL));
    CHECK(!mount("tmpfs","/mp/b/x","tmpfs",0,NULL));touch("/mp/b/x/preserved");
    CHECK(!umount("/mp/a/x"));marker(AT_FDCWD,"/mp/b/x/preserved");
    CHECK(!umount("/mp/b/x"));
    puts("PROP CASE incoming-overmount-routing-and-promotion PASS");
    CHECK(!mount("tmpfs","/mp/u","tmpfs",0,NULL));touch("/mp/u/live");
    int moved=open("/mp/u/live",O_RDONLY);CHECK(moved>=0);
    CHECK(!mount("/mp/u","/mp/a/y",NULL,MS_MOVE,NULL));
    marker(AT_FDCWD,"/mp/b/y/live");marker(AT_FDCWD,"/mp/c/y/live");marker(AT_FDCWD,"/mp/d/y/live");
    ERROR(mount("/mp/a/y","/mp/u",NULL,MS_MOVE,NULL),EINVAL);
    char moved_byte;CHECK(read(moved,&moved_byte,1)==1&&moved_byte=='x'&&!close(moved)&&!umount("/mp/a/y"));
    make_directory("/mp/a/sub");make_directory("/mp/a/sub/point");
    CHECK(!mount("/mp/a/sub","/mp/r",NULL,MS_BIND,NULL));
    CHECK(mount_row("/mp/r",&a)&&!strcmp(a.root,"/sub"));
    CHECK(!mount("tmpfs","/mp/a/sub/point","tmpfs",0,NULL));touch("/mp/a/sub/point/marker");
    marker(AT_FDCWD,"/mp/r/point/marker");CHECK(!umount("/mp/r/point")&&!umount("/mp/r"));
    FILE *thread_info=fopen("/mp/proc/thread-self/mountinfo","r");CHECK(thread_info);
    unsigned long thread_id;CHECK(fscanf(thread_info,"%lu",&thread_id)==1&&thread_id&&!fclose(thread_info));
    ERROR(mount(NULL,"/mp/a",NULL,MS_SHARED|MS_PRIVATE,NULL),EINVAL);
    puts("PROP CASE move-subdirectory-bind-root-and-thread-mountinfo PASS");
    CHECK(!mount("tmpfs","/mp/u","tmpfs",0,NULL));make_directory("/mp/u/skip");
    CHECK(!mount("tmpfs","/mp/u/skip","tmpfs",0,NULL));touch("/mp/u/skip/hidden");
    CHECK(!mount(NULL,"/mp/u/skip",NULL,MS_UNBINDABLE,NULL));
    CHECK(mount_row("/mp/u/skip",&c)&&c.unbindable);ERROR(mount("/mp/u/skip","/mp/r",NULL,MS_BIND,NULL),EINVAL);
    CHECK(!mount("/mp/u","/mp/r",NULL,MS_BIND|MS_REC,NULL));ERROR(access("/mp/r/skip/hidden",F_OK),ENOENT);
    CHECK(!mount(NULL,"/mp/r",NULL,MS_PRIVATE|MS_REC,NULL)&&!umount("/mp/r"));
    puts("PROP CASE unbindable-rbind-pruning-and-private PASS");
    int ready[2],go[2];CHECK(!pipe(ready)&&!pipe(go));pid_t child=fork();CHECK(child>=0);
    if(!child){
        CHECK(!unshare(CLONE_NEWNS));CHECK(write(ready[1],"r",1)==1);char x;CHECK(read(go[0],&x,1)==1);
        marker(AT_FDCWD,"/mp/a/y/marker");
        CHECK(!mount("tmpfs","/mp/b/nschild","tmpfs",0,NULL));touch("/mp/b/nschild/marker");
        CHECK(write(ready[1],"d",1)==1&&read(go[0],&x,1)==1);_exit(0);
    }
    char x;CHECK(read(ready[0],&x,1)==1);
    CHECK(!mount("tmpfs","/mp/a/y","tmpfs",0,NULL));touch("/mp/a/y/marker");
    CHECK(write(go[1],"g",1)==1&&read(ready[0],&x,1)==1);marker(AT_FDCWD,"/mp/a/nschild/marker");
    CHECK(snprintf(path,sizeof(path),"/mp/proc/%d/ns/mnt",(int)child)>0);
    int other=open(path,O_RDONLY),original=open("/mp/proc/self/ns/mnt",O_RDONLY);CHECK(other>=0&&original>=0);
    CHECK(write(go[1],"e",1)==1);joined(child);
    CHECK(!mount("tmpfs","/mp/a/post","tmpfs",0,NULL));touch("/mp/a/post/marker");
    CHECK(!setns(other,CLONE_NEWNS));
    marker(AT_FDCWD,"/mp/a/post/marker");
    CHECK(!setns(original,CLONE_NEWNS)&&!close(other)&&!close(original));
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(go[0])&&!close(go[1]));
    puts("PROP CASE namespace-peer-copy-retained-ns-and-last-owner PASS");
    propagation_edges();
    CHECK(!mount("tmpfs","/mp/a/x","tmpfs",0,NULL));touch("/mp/a/x/held");
    int held=open("/mp/b/x/held",O_RDONLY);CHECK(held>=0);ERROR(umount("/mp/a/x"),EBUSY);
    CHECK(!umount2("/mp/a/x",MNT_DETACH));CHECK(!mount_row("/mp/b/x",&b));
    CHECK(read(held,&x,1)==1&&x=='x'&&!close(held));
    CHECK(!mount(NULL,"/mp",NULL,MS_PRIVATE|MS_REC,NULL)&&!umount2("/mp",MNT_DETACH)&&!rmdir("/mp"));
    puts("PROP CASE propagated-busy-lazy-detach-and-open-owner PASS");
    puts("PROP END 9");return 0;
}
int main(void)
{
#ifndef MOUNT_PROPAGATION_ONLY
#define MOUNT_PROPAGATION_ONLY 0
#endif
    if (MOUNT_PROPAGATION_ONLY) return propagation();
    CHECK(!mkdir("/ns-proc",0755) && !mount("proc","/ns-proc","proc",0,NULL));
    CHECK(!mkdir(data_path,0755) && !mount("tmpfs",data_path,"tmpfs",0,NULL));
    CHECK(!mkdir(nested_path,0755) && !mount("tmpfs",nested_path,"tmpfs",0,NULL));
    CHECK(!mkdir(private_path,0755));touch(marker_path);
    int original=open(self_ns,O_RDONLY|O_CLOEXEC);
    int original_dir=open(nested_path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    CHECK(original>=0 && original_dir>=0 && (fcntl(original,F_GETFD)&FD_CLOEXEC));
    struct stat oldstat,newstat;
    CHECK(!fstat(original,&oldstat));
    struct statfs ns_statfs;
    CHECK(!fstatfs(original,&ns_statfs) && ns_statfs.f_type==0x6e736673);
    CHECK(ioctl(original,0xb703U)==CLONE_NEWNS);
    ERROR(read(original,&newstat,1),EINVAL);
    char text[64];ssize_t length=readlink(self_ns,text,sizeof(text)-1);
    CHECK(length>6);text[length]=0;CHECK(!strncmp(text,"mnt:[",5));
    CHECK(!chdir(nested_path));

    int ready[2],release[2];
    CHECK(!pipe(ready) && !pipe(release));
    pid_t parent=getpid(), child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(!unshare(CLONE_NEWNS));
        CHECK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));
        CHECK(getcwd(text,sizeof(text)) && !strcmp(text,nested_path));
        marker(AT_FDCWD,"marker");
        ERROR(umount(nested_path),EBUSY);
        CHECK(!fchdir(original_dir));
        marker(AT_FDCWD,"marker");
        ERROR(mount("tmpfs",".","tmpfs",0,NULL),EINVAL);
        CHECK(!chdir("/"));
        int local=open(self_ns,O_RDONLY);CHECK(local>=0 && !fstat(local,&newstat));
        CHECK(oldstat.st_ino!=newstat.st_ino && !close(local));
        CHECK(!mount("tmpfs",private_path,"tmpfs",0,NULL));touch(hidden_path);
        CHECK(mount_listing(getpid()," /ns-data/private ") && !mount_listing(parent," /ns-data/private "));
        CHECK(write(ready[1],"x",1)==1 && read(release[0],text,1)==1);
        _exit(0);
    }
    CHECK(read(ready[0],text,1)==1);
    ERROR(access(hidden_path,F_OK),ENOENT);
    CHECK(!mount_listing(getpid()," /ns-data/private ") && mount_listing(child," /ns-data/private "));
    CHECK(snprintf(text,sizeof(text),"/ns-proc/%d/ns/mnt",(int)child)>0);
    int copy=open(text,O_RDONLY|O_CLOEXEC);CHECK(copy>=0);
    CHECK(!fstat(copy,&newstat) && newstat.st_ino!=oldstat.st_ino);
    int pidfd=(int)syscall(SYS_pidfd_open,child,0);CHECK(pidfd>=0);
    ERROR(setns(pidfd,0),EINVAL);
    CHECK(!setns(pidfd,CLONE_NEWNS) && getcwd(text,sizeof(text)) && !strcmp(text,"/"));
    marker(AT_FDCWD,hidden_path);
    CHECK(!setns(original,0) && !close(pidfd));
    ERROR(access(hidden_path,F_OK),ENOENT);
    CHECK(write(release[1],"x",1)==1);joined(child);
    CHECK(!close(ready[0]) && !close(ready[1]) && !close(release[0]) && !close(release[1]));
    /* Only the copied namespace FD owns the tree after the child exits. */
    CHECK(!setns(copy,0));marker(AT_FDCWD,hidden_path);
    CHECK(!setns(original,CLONE_NEWNS) && !close(copy));
    ERROR(access(hidden_path,F_OK),ENOENT);
    puts("MOUNTNS CASE unshare-cwd-isolation-proc-setns-last-owner PASS");

    ERROR(clone(clone_isolate,clone_stack+sizeof(clone_stack),CLONE_NEWNS|CLONE_FS|SIGCHLD,NULL),EINVAL);
    child=clone(clone_isolate,clone_stack+sizeof(clone_stack),CLONE_NEWNS|SIGCHLD,NULL);
    CHECK(child>=0);joined(child);
    marker(AT_FDCWD,marker_path);
    child=clone(clone_share,clone_stack+sizeof(clone_stack),CLONE_FS|SIGCHLD,&original);
    CHECK(child>=0);joined(child);
    child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(!setuid(65534));
        ERROR(unshare(CLONE_NEWNS),EPERM);
        ERROR(setns(original,CLONE_NEWNS),EPERM);
        _exit(0);
    }
    joined(child);
    puts("MOUNTNS CASE clone-flags-shared-fs-permissions PASS");
    CHECK(!close(original_dir) && !close(original));
    CHECK(!umount(nested_path) && !umount(data_path) && !umount("/ns-proc"));
    CHECK(!rmdir(data_path) && !rmdir("/ns-proc"));
    puts("MOUNTNS END 2");
    return 0;
}
