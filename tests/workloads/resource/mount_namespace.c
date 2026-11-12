#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/resource.h>
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
struct native_mount_attr { uint64_t set,clear,propagation,userns; };
static int tree_open(int fd,const char *path,unsigned flags)
{ return (int)syscall(428,fd,path,flags); }
static int tree_move(int from,const char *a,int to,const char *b,unsigned flags)
{ return (int)syscall(429,from,a,to,b,flags); }
static int tree_attr(int fd,const char *path,unsigned flags,uint64_t set,uint64_t clear,uint64_t propagation)
{
    struct native_mount_attr attr={set,clear,propagation,0};
    return (int)syscall(442,fd,path,flags,&attr,sizeof(attr));
}
static int fs_open(const char *type,unsigned flags)
{ return (int)syscall(430,type,flags); }
static int fs_config(int fd,unsigned command,const char *key,const void *value,int aux)
{ return (int)syscall(431,fd,command,key,value,aux); }
static int fs_mount(int fd,unsigned flags,unsigned attributes)
{ return (int)syscall(432,fd,flags,attributes); }
static int fs_pick(int fd,const char *path,unsigned flags)
{ return (int)syscall(433,fd,path,flags); }
static int filesystem_api(void)
{
    ERROR(fs_open("tmpfs",2),EINVAL);ERROR(fs_open("not-a-filesystem",0),ENODEV);
    ERROR(fs_open((void *)1,0),EFAULT);
    ERROR(fs_config(-1,6,NULL,NULL,0),EINVAL);
    ERROR(fs_config(999,6,NULL,NULL,0),EBADF);
    ERROR(fs_config(999,99,NULL,NULL,0),EOPNOTSUPP);
    ERROR(fs_config(999,1,NULL,"x",0),EINVAL);
    ERROR(fs_mount(-1,0,0),EBADF);ERROR(fs_mount(-1,4,0),EINVAL);
    ERROR(fs_mount(-1,0,0x40),EINVAL);
    int context=fs_open("tmpfs",1);CHECK(context>=0);
    CHECK(fcntl(context,F_GETFD)&FD_CLOEXEC);
    CHECK((fcntl(context,F_GETFL)&O_ACCMODE)==O_RDWR);
    struct stat st;CHECK(!fstat(context,&st)&&(st.st_mode&0777)==0600&&!(st.st_mode&S_IFMT));
    struct statfs anonymous;CHECK(!fstatfs(context,&anonymous)&&anonymous.f_type==0x09041934);
    char byte;ERROR(read(context,&byte,0),ENODATA);ERROR(read(context,&byte,1),ENODATA);ERROR(write(context,"x",1),EINVAL);
    ERROR(pread(context,&byte,1,0),ESPIPE);ERROR(lseek(context,0,SEEK_SET),ESPIPE);
    struct pollfd event={.fd=context,.events=POLLIN|POLLOUT};
    CHECK(poll(&event,1,0)==1&&(event.revents&(POLLIN|POLLOUT))==(POLLIN|POLLOUT));
    int epoll=epoll_create1(0);CHECK(epoll>=0);struct epoll_event registration={.events=EPOLLIN};
    ERROR(epoll_ctl(epoll,EPOLL_CTL_ADD,context,&registration),EPERM);CHECK(!close(epoll));
    CHECK(!mkdir("/context-proc",0755)&&!mount("proc","/context-proc","proc",0,NULL));
    char fd_name[80],display[80];CHECK(snprintf(fd_name,sizeof(fd_name),"/context-proc/self/fd/%d",context)>0);
    ssize_t display_len=readlink(fd_name,display,sizeof(display)-1);CHECK(display_len>0);display[display_len]=0;
    CHECK(!strcmp(display,"anon_inode:[fscontext]")&&!umount("/context-proc")&&!rmdir("/context-proc"));
    ERROR(fs_mount(context,0,0),EINVAL);
    ERROR(fs_config(context,1,(void *)1,"0",0),EFAULT);
    ERROR(fs_config(context,5,"source",NULL,999),EBADF);
    ERROR(fs_config(context,1,"unknown-option","x",0),EINVAL);
    ERROR(fs_config(context,1,"mode","0700,size=4096",0),EINVAL);
    ERROR(fs_config(context,1,"size","64K,mode=0777",0),EINVAL);
    int devpts_context=fs_open("devpts",0);CHECK(devpts_context>=0);
    ERROR(fs_config(devpts_context,1,"newinstance","anything",0),EINVAL);
    ERROR(fs_config(devpts_context,1,"mode","0620,uid=1",0),EINVAL);
    CHECK(!fs_config(devpts_context,0,"newinstance",NULL,0)&&!close(devpts_context));
    char long_mode[256];memset(long_mode,'0',sizeof(long_mode));memcpy(long_mode+252,"700",4);
    CHECK(!fs_config(context,1,"mode",long_mode,0));
    CHECK(!fs_config(context,1,"size","64K",0));
    CHECK(!fs_config(context,1,"mode","0700",0));
    CHECK(!fs_config(context,1,"nr_inodes","32",0));
    CHECK(!fs_config(context,6,NULL,NULL,0));
    ERROR(fs_config(context,1,"mode","0777",0),EBUSY);
    int view=fs_mount(context,1,2|4|8);CHECK(view>=0);
    CHECK(fcntl(view,F_GETFD)&FD_CLOEXEC);
    touch("/wrong-mount-point");
    ERROR(tree_move(view,"",AT_FDCWD,"/wrong-mount-point",4),EINVAL);
    CHECK(!unlink("/wrong-mount-point")&&!mkdir("/fc",0755));
    CHECK(!tree_move(view,"",AT_FDCWD,"/fc",4));
    CHECK(!stat("/fc",&st)&&(st.st_mode&07777)==0700);
    struct statfs stats;CHECK(!statfs("/fc",&stats)&&stats.f_blocks==65536/(unsigned long)sysconf(_SC_PAGESIZE));
    int duplicate=dup(context);CHECK(duplicate>=0&&!close(context));context=duplicate;
    ERROR(fs_config(context,6,NULL,NULL,0),EBUSY);
    CHECK(!close(context)&&!close(view));touch("/fc/marker");
    ERROR(mount("/missing-mount-source","/fc/marker","ext4",0,NULL),ENOENT);
    ERROR(mount("tmpfs","/fc/marker","tmpfs",0,NULL),ENOTDIR);
    puts("FS-CONTEXT CASE import-state-detached-view-and-dup PASS");

    ERROR(fs_pick(AT_FDCWD,"/fc/marker",0),EINVAL);
    int pick=fs_pick(AT_FDCWD,"/fc",1);CHECK(pick>=0);
    ERROR(fs_mount(pick,0,0),EBUSY);
    CHECK(!fs_config(pick,0,"ro",NULL,0));
    CHECK(!fs_config(pick,7,NULL,NULL,0));
    ERROR(open("/fc/new",O_CREAT|O_WRONLY,0600),EROFS);
    CHECK(!fs_config(pick,0,"rw",NULL,0));CHECK(!fs_config(pick,7,NULL,NULL,0));
    touch("/fc/new");CHECK(!close(pick));
    CHECK(!mount(NULL,"/fc",NULL,MS_REMOUNT|MS_RDONLY,NULL));
    pick=fs_pick(AT_FDCWD,"/fc",0);CHECK(pick>=0);
    CHECK(!fs_config(pick,0,"rw",NULL,0)&&!fs_config(pick,7,NULL,NULL,0)&&!close(pick));
    ERROR(open("/fc/legacy",O_CREAT|O_WRONLY,0600),EROFS);
    CHECK(!tree_attr(AT_FDCWD,"/fc",0,0,1,0));
    touch("/fc/legacy");puts("FS-CONTEXT CASE reconfigure-and-legacy-mount PASS");

    context=fs_open("tmpfs",0);CHECK(context>=0&&!fs_config(context,6,NULL,NULL,0));
    int ready[2],go[2];CHECK(!pipe(ready)&&!pipe(go));pid_t child=fork();CHECK(child>=0);
    if(!child){
        CHECK(!close(ready[0])&&!close(go[1])&&!unshare(CLONE_NEWNS));
        int fd=fs_mount(context,0,0);CHECK(fd>=0&&!close(context));
        CHECK(write(ready[1],"r",1)==1&&read(go[0],&byte,1)==1);
        CHECK(!mkdir("/child-fc",0755)&&!tree_move(fd,"",AT_FDCWD,"/child-fc",4));
        touch("/child-fc/alive");CHECK(!close(fd)&&!umount("/child-fc")&&!rmdir("/child-fc"));_exit(0);
    }
    CHECK(!close(ready[1])&&!close(go[0])&&read(ready[0],&byte,1)==1&&!close(context));
    CHECK(write(go[1],"g",1)==1);joined(child);
    CHECK(!close(ready[0])&&!close(go[1]));ERROR(access("/child-fc",F_OK),ENOENT);
    puts("FS-CONTEXT CASE fork-namespace-and-last-config-close PASS");

    context=fs_open("tmpfs",0);CHECK(context>=0&&!fs_config(context,6,NULL,NULL,0));
    int ns=fs_mount(context,3,0);CHECK(ns>=0&&!close(context));
    child=fork();CHECK(child>=0);
    if(!child){CHECK(!setns(ns,CLONE_NEWNS));touch("/from-context");_exit(0);}
    joined(child);CHECK(!close(ns));ERROR(access("/from-context",F_OK),ENOENT);
    puts("FS-CONTEXT CASE namespace-fd PASS");

    context=fs_open("ext4",0);CHECK(context>=0);
    CHECK(!fs_config(context,1,"source","/does-not-exist",0));
    ERROR(fs_config(context,6,NULL,NULL,0),ENOENT);
    ERROR(fs_config(context,1,"source","/",0),EBUSY);CHECK(!close(context));
    context=fs_open("tmpfs",0);CHECK(context>=0);
    ERROR(fs_config(context,2,"size","x",0),EINVAL);
    ERROR(fs_config(context,2,"size",(void *)1,4),EFAULT);
    ERROR(fs_config(context,5,"source",NULL,999),EBADF);
    CHECK(!fs_config(context,8,NULL,NULL,0));
    int discarded=fs_mount(context,0,0);CHECK(discarded>=0&&!close(discarded)&&!close(context));
    puts("FS-CONTEXT CASE failed-create-and-unattached-cleanup PASS");

    struct stat original;CHECK(!stat("/",&original));
    dev_t root_device=original.st_dev;
    CHECK(!mknod("/context-device",S_IFBLK|0600,root_device)&&!mkdir("/context-disk",0755));
    context=fs_open("ext4",0);CHECK(context>=0&&!fs_config(context,1,"source","/context-device",0));
    ERROR(fs_config(context,8,NULL,NULL,0),EBUSY);CHECK(!close(context));
    context=fs_open("ext4",0);CHECK(context>=0&&!fs_config(context,1,"source","/context-device",0));
    CHECK(!fs_config(context,6,NULL,NULL,0));view=fs_mount(context,0,1);CHECK(view>=0&&!close(context));
    CHECK(!tree_move(view,"",AT_FDCWD,"/context-disk",4));
    CHECK(!stat("/context-disk",&st)&&st.st_dev==original.st_dev&&st.st_ino==original.st_ino);
    ERROR(open("/context-disk/blocked",O_CREAT|O_WRONLY,0600),EROFS);
    touch("/context-root-rw");CHECK(!unlink("/context-root-rw"));
    CHECK(!close(view)&&!umount("/context-disk")&&!rmdir("/context-disk")&&!unlink("/context-device"));
    puts("FS-CONTEXT CASE block-instance-reuse-exclusive-and-view-readonly PASS");

    CHECK(syscall(151,1000)==0);context=fs_open("tmpfs",0);CHECK(context>=0);
    CHECK(syscall(151,0)==1000&&!fs_config(context,6,NULL,NULL,0));
    view=fs_mount(context,0,0);CHECK(view>=0&&!fstat(view,&st)&&st.st_uid==1000&&!close(view)&&!close(context));
    context=fs_open("tmpfs",0);CHECK(context>=0);child=fork();CHECK(child>=0);
    if(!child){CHECK(!setuid(1000));ERROR(fs_open("tmpfs",0),EPERM);ERROR(fs_config(context,6,NULL,NULL,0),EPERM);_exit(0);}
    joined(child);CHECK(!fs_config(context,6,NULL,NULL,0));
    struct rlimit old_limit,capped;CHECK(!getrlimit(RLIMIT_NOFILE,&old_limit));capped=old_limit;capped.rlim_cur=32;
    CHECK(!setrlimit(RLIMIT_NOFILE,&capped));int occupied[32],used=0,extra;
    while((extra=dup(context))>=0){CHECK(used<32);occupied[used++]=extra;}
    CHECK(errno==EMFILE);ERROR(fs_open("tmpfs",0),EMFILE);ERROR(fs_mount(context,0,0),EMFILE);
    while(used)CHECK(!close(occupied[--used]));
    CHECK(!setrlimit(RLIMIT_NOFILE,&old_limit));
    ERROR(fs_mount(context,0,0),EBUSY);CHECK(!close(context));
    puts("FS-CONTEXT CASE creator-credentials-capability-and-fd-exhaustion PASS");

    context=fs_open("tmpfs",0);CHECK(context>=0&&!pipe(ready)&&!pipe(go));
    pid_t racers[2];
    for(unsigned i=0;i<2;i++){
        racers[i]=fork();CHECK(racers[i]>=0);
        if(!racers[i]){CHECK(!close(ready[0])&&!close(go[1])&&read(go[0],&byte,1)==1);
            int result=fs_config(context,6,NULL,NULL,0);result=result<0?-errno:0;
            CHECK(write(ready[1],&result,sizeof(result))==sizeof(result));_exit(0);}
    }
    CHECK(!close(ready[1])&&!close(go[0])&&write(go[1],"gg",2)==2);
    int results[2];CHECK(read(ready[0],&results[0],sizeof(int))==sizeof(int)&&read(ready[0],&results[1],sizeof(int))==sizeof(int));
    joined(racers[0]);joined(racers[1]);CHECK(!close(ready[0])&&!close(go[1]));
    CHECK((results[0]==0&&results[1]==-EBUSY)||(results[1]==0&&results[0]==-EBUSY));
    view=fs_mount(context,0,0);CHECK(view>=0&&!close(view)&&!close(context));
    puts("FS-CONTEXT CASE shared-context-single-create PASS");

    CHECK(!mkdir("/picked-unmount",0755)&&!mount("tmpfs","/picked-unmount","tmpfs",0,NULL));
    pick=fs_pick(AT_FDCWD,"/picked-unmount",0);CHECK(pick>=0);
    CHECK(!umount("/picked-unmount")&&!rmdir("/picked-unmount"));
    CHECK(!fs_config(pick,0,"ro",NULL,0)&&!fs_config(pick,7,NULL,NULL,0)&&!close(pick));
    puts("FS-CONTEXT CASE pick-retains-instance-without-pinning-namespace-view PASS");

    CHECK(!unlink("/fc/marker")&&!unlink("/fc/new")&&!unlink("/fc/legacy")&&!umount("/fc")&&!rmdir("/fc"));
    puts("FS-CONTEXT END 9");return 0;
}
static int mount_api(void)
{
    CHECK(!mkdir("/ma",0755)&&!mount("tmpfs","/ma","tmpfs",0,NULL));
    CHECK(!mkdir("/ma/source",0755)&&!mkdir("/ma/target",0755)&&!mkdir("/ma/other",0755));
    CHECK(!mount("tmpfs","/ma/source","tmpfs",0,NULL));
    touch("/ma/source/marker");CHECK(!mkdir("/ma/source/sub",0755));
    CHECK(!mount("tmpfs","/ma/source/sub","tmpfs",0,NULL));touch("/ma/source/sub/child");
    int path=tree_open(AT_FDCWD,"/ma/source",O_CLOEXEC);
    CHECK(path>=0&&(fcntl(path,F_GETFL)&O_PATH)&&(fcntl(path,F_GETFD)&FD_CLOEXEC));
    struct stat a,b;CHECK(!fstat(path,&a)&&!stat("/ma/source",&b)&&a.st_ino==b.st_ino&&a.st_dev==b.st_dev);
    ERROR(tree_open(AT_FDCWD,"/ma/source",0x40000000U),EINVAL);
    ERROR(tree_open(path,"",0),ENOENT);
    int same=tree_open(path,"",AT_EMPTY_PATH);CHECK(same>=0&&!close(same));
    int generated=tree_open(AT_FDCWD,"/proc/meminfo",0);CHECK(generated>=0);
    char zero;ERROR(read(generated,&zero,1),EBADF);CHECK(!close(generated));
    generated=open("/proc/meminfo",O_PATH);CHECK(generated>=0);ERROR(read(generated,&zero,1),EBADF);CHECK(!close(generated));
    ERROR(tree_open(AT_FDCWD,"/ma/source/marker",2U),ENOTDIR);
    struct rlimit limit,small;CHECK(!getrlimit(RLIMIT_NOFILE,&limit));small=limit;small.rlim_cur=32;
    CHECK(!setrlimit(RLIMIT_NOFILE,&small));
    int filled[32],n=0,fd;
    while((fd=dup(path))>=0){CHECK(n<32);filled[n++]=fd;}
    CHECK(errno==EMFILE);ERROR(tree_open(-1,"missing",0x40000000U),EMFILE);
    while(n)CHECK(!close(filled[--n]));
    CHECK(!setrlimit(RLIMIT_NOFILE,&limit));
    int ns=tree_open(path,"",2U|AT_EMPTY_PATH|O_CLOEXEC);CHECK(ns>=0);
    pid_t ns_child=fork();CHECK(ns_child>=0);
    if(!ns_child){CHECK(!setns(ns,CLONE_NEWNS));marker(AT_FDCWD,"/marker");_exit(0);}
    joined(ns_child);CHECK(!close(ns));
    CHECK(!tree_attr(path,"",AT_EMPTY_PATH,0,0,MS_SHARED));
    int detached=tree_open(path,"",1U|AT_EMPTY_PATH|AT_RECURSIVE);CHECK(detached>=0);
    ns_child=fork();CHECK(ns_child>=0);
    if(!ns_child) {
        CHECK(!unshare(CLONE_NEWNS));
        ERROR(tree_open(path,"",1U|AT_EMPTY_PATH),EINVAL);
        ERROR(tree_open(detached,"",1U|AT_EMPTY_PATH),EINVAL);
        int local=tree_open(AT_FDCWD,"/ma/source",0),clone=tree_open(local,"",1U|AT_EMPTY_PATH|AT_RECURSIVE);
        CHECK(local>=0&&clone>=0);
        ERROR(tree_move(local,"",clone,"",4U|64U),EINVAL);
        int sub=openat(clone,"sub",O_PATH|O_DIRECTORY);CHECK(sub>=0);
        ERROR(tree_move(sub,"",AT_FDCWD,"/ma/target",4U),EINVAL);
        ERROR(tree_attr(sub,"",AT_EMPTY_PATH,2U,0,0),EINVAL);
        CHECK(!tree_attr(local,"",AT_EMPTY_PATH,0,0,MS_PRIVATE));
        CHECK(!tree_move(path,"",local,"",4U|64U|0x100U));
        CHECK(!close(sub)&&!close(clone)&&!close(local));_exit(0);
    }
    joined(ns_child);CHECK(!close(detached));
    puts("MOUNT-API CASE path-fd-and-input PASS");

    int tree=tree_open(path,"",1U|AT_EMPTY_PATH|AT_RECURSIVE|O_CLOEXEC);
    CHECK(tree>=0);same=dup(tree);CHECK(same>=0&&!close(tree));tree=same;
    int old=openat(tree,"marker",O_RDONLY);CHECK(old>=0);
    CHECK(!tree_attr(tree,"",AT_EMPTY_PATH|AT_RECURSIVE,1U|2U|4U|8U,0,MS_PRIVATE));
    struct statfs sf;CHECK(!fstatfs(tree,&sf)&&(sf.f_flags&1U));
    ERROR(openat(tree,"new",O_CREAT|O_WRONLY,0600),EROFS);
    CHECK(!tree_attr(tree,"",AT_EMPTY_PATH|AT_RECURSIVE,0,1U,0));
    CHECK(!tree_attr(path,"",AT_EMPTY_PATH,0,0,MS_SHARED));
    CHECK(!tree_move(path,"",tree,"",4U|64U|0x100U));
    int filetree=tree_open(path,"marker",1U);CHECK(filetree>=0);
    CHECK(!tree_attr(filetree,"",AT_EMPTY_PATH,0,0,MS_PRIVATE));
    CHECK(!tree_move(path,"",filetree,"",4U|64U|0x100U)&&!close(filetree));
    CHECK(!tree_move(tree,"",AT_FDCWD,"/ma/target",4U));
    marker(AT_FDCWD,"/ma/target/marker");marker(AT_FDCWD,"/ma/target/sub/child");
    CHECK(!close(tree));marker(AT_FDCWD,"/ma/target/marker");
    char c;CHECK(read(old,&c,1)==1&&c=='x'&&!close(old));
    CHECK(!tree_attr(AT_FDCWD,"/ma/target",AT_RECURSIVE,0,0,MS_PRIVATE));
    CHECK(!umount2("/ma/target",MNT_DETACH));
    tree=tree_open(path,"",1U|AT_EMPTY_PATH);CHECK(tree>=0);
    int hidden=open("/ma/target",O_PATH|O_DIRECTORY);CHECK(hidden>=0);
    CHECK(!mount("tmpfs","/ma/target","tmpfs",0,NULL));touch("/ma/target/top");
    ERROR(tree_move(tree,"",hidden,"",4U|64U|0x200U),EINVAL);CHECK(!close(hidden));
    CHECK(!tree_move(tree,"",AT_FDCWD,"/ma/target",4U|0x200U)&&!close(tree));
    marker(AT_FDCWD,"/ma/target/top");ERROR(access("/ma/target/marker",F_OK),ENOENT);
    CHECK(!umount("/ma/target"));marker(AT_FDCWD,"/ma/target/marker");CHECK(!umount("/ma/target"));
    puts("MOUNT-API CASE clone-recursive-attributes-move-and-close PASS");

    CHECK(!tree_attr(path,"",AT_EMPTY_PATH|AT_RECURSIVE,0,2U|4U|8U,0));
    int writer=open("/ma/source/sub/writer",O_CREAT|O_WRONLY,0600);CHECK(writer>=0);
    ERROR(tree_attr(path,"",AT_EMPTY_PATH|AT_RECURSIVE,1U,0,0),EBUSY);
    CHECK(!fstatfs(path,&sf)&&!(sf.f_flags&1U));
    CHECK(!close(writer));
    CHECK(!tree_attr(path,"",AT_EMPTY_PATH,0x10U,0x70U,0));
    CHECK(!fstatfs(path,&sf)&&(sf.f_flags&1024U));
    ERROR(tree_attr(path,"",AT_EMPTY_PATH,0,0x10U,0),EINVAL);
    struct native_mount_attr noop={0};
    CHECK(!syscall(442,-1,(void *)1,0,&noop,sizeof(noop)));
    ERROR(syscall(442,path,"",AT_EMPTY_PATH,&noop,31),EINVAL);
    struct {struct native_mount_attr value;uint64_t tail;} extended={.tail=1};
    ERROR(syscall(442,path,"",AT_EMPTY_PATH,&extended,sizeof(extended)),E2BIG);
    ERROR(syscall(442,path,"",AT_EMPTY_PATH,(void *)1,32),EFAULT);
    ERROR(tree_attr(path,"",AT_EMPTY_PATH,0x10U,0,0),EINVAL);
    ERROR(tree_attr(path,"",AT_EMPTY_PATH,0,0,MS_SHARED|MS_PRIVATE),EINVAL);
    CHECK(!tree_attr(path,"",AT_EMPTY_PATH,0,0x70U,0));
    CHECK(!tree_move(AT_FDCWD,"/ma/source",AT_FDCWD,"/ma/other",0));
    marker(path,"marker");marker(AT_FDCWD,"/ma/other/sub/child");CHECK(!close(path));
    CHECK(!umount2("/ma/other",MNT_DETACH));
    puts("MOUNT-API CASE atomic-recursive-busy-and-source-fd PASS");

    CHECK(!mount("tmpfs","/ma/source","tmpfs",0,NULL));touch("/ma/source/marker");
    tree=tree_open(AT_FDCWD,"/ma/source",1U);CHECK(tree>=0);
    int held=openat(tree,"marker",O_RDONLY);CHECK(held>=0&&!close(tree));
    CHECK(read(held,&c,1)==1&&c=='x'&&!close(held));
    CHECK(!umount("/ma/source"));
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!unshare(CLONE_NEWNS)&&!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));
        CHECK(!mount("tmpfs","/ma/target","tmpfs",0,NULL));
        CHECK(!mkdir("/ma/target/old",0755));touch("/ma/target/mark");
        int oldroot=open("/",O_PATH|O_DIRECTORY);CHECK(oldroot>=0&&!chdir("/"));
        int entered[2],go[2];CHECK(!pipe(entered)&&!pipe(go));
        pid_t observer=fork();CHECK(observer>=0);
        if(!observer) {
            int info=open("/proc/self/mountinfo",O_RDONLY);CHECK(info>=0);
            char text[4096],byte,cwd[32];CHECK(read(info,text,sizeof(text))>0);
            CHECK(write(entered[1],"x",1)==1&&read(go[0],&byte,1)==1);
            CHECK(getcwd(cwd,sizeof(cwd))&&!strcmp(cwd,"/"));marker(AT_FDCWD,"/mark");
            /* Linux binds a mountinfo FD to its open-time root. Use a fresh
             * FD when asking about the process root after the pivot. */
            CHECK(!close(info));info=open("/old/proc/self/mountinfo",O_RDONLY);CHECK(info>=0);
            ssize_t bytes=read(info,text,sizeof(text)-1);CHECK(bytes>0);text[bytes]=0;
            CHECK(strstr(text," /old ")&&!close(info));_exit(0);
        }
        char byte;CHECK(read(entered[0],&byte,1)==1);
        CHECK(!syscall(41,"/ma/target","/ma/target/old"));
        CHECK(write(go[1],"x",1)==1);joined(observer);
        CHECK(!close(entered[0])&&!close(entered[1])&&!close(go[0])&&!close(go[1]));
        char cwd[32];CHECK(getcwd(cwd,sizeof(cwd))&&!strcmp(cwd,"/"));
        marker(AT_FDCWD,"/mark");CHECK(!access("/old/ma/target",F_OK));
        CHECK(!fstat(oldroot,&a)&&!stat("/old",&b)&&a.st_ino==b.st_ino&&a.st_dev==b.st_dev);
        CHECK(!close(oldroot));_exit(0);
    }
    joined(child);CHECK(!access("/ma/target",F_OK));
    puts("MOUNT-API CASE detached-last-owner-and-pivot-root PASS");
    child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!setuid(65534));ERROR(tree_open(AT_FDCWD,"/ma",1U),EPERM);
        ERROR(tree_move(-1,"",-1,"",4U|64U),EPERM);
        ERROR(tree_attr(-1,"",AT_EMPTY_PATH,1,0,0),EPERM);
        ERROR(syscall(41,"/", "/"),EPERM);
        ERROR(syscall(41,"/missing-root","/missing-old"),ENOENT);_exit(0);
    }
    joined(child);CHECK(!umount("/ma")&&!rmdir("/ma"));
    /* Exercise the initial namespace as well as the copied-tree child. The
     * supervisor and existing executable mappings retain their old objects. */
    CHECK(!mkdir("/final-root",0755)&&!mount("tmpfs","/final-root","tmpfs",0,NULL));
    CHECK(!mkdir("/final-root/old",0755));touch("/final-root/alive");
    CHECK(!chdir("/")&&!syscall(41,"/final-root","/final-root/old"));
    char cwd[32];CHECK(getcwd(cwd,sizeof(cwd))&&!strcmp(cwd,"/"));
    marker(AT_FDCWD,"/alive");CHECK(!access("/old",F_OK));
    puts("MOUNT-API CASE permissions-and-cleanup PASS");
    puts("MOUNT-API END 5");return 0;
}
int main(void)
{
#ifdef MOUNT_CONTEXT_ONLY
    return filesystem_api();
#else
    (void)filesystem_api;
#endif
#ifdef MOUNT_API_ONLY
    return mount_api();
#else
    (void)mount_api;
#endif
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
