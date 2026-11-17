#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <signal.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FALLOC FAIL %d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
#define FAIL(e,x) do{errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static void state(int fd,off_t size,blkcnt_t blocks)
{struct stat s;CHECK(!fstat(fd,&s));CHECK(s.st_size==size&&s.st_blocks==blocks);}
static void contract(const char *name,int quota)
{
 long p=sysconf(_SC_PAGESIZE);int f=open(name,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(f>=0);
 FAIL(EINVAL,fallocate(f,0,0,0));FAIL(EOPNOTSUPP,fallocate(f,2,0,p));
 CHECK(!fallocate(f,1,p+1,2*p));state(f,0,3*p/512);
 CHECK(!fallocate(f,0,p+1,2*p));state(f,3*p+1,3*p/512);
 char z[32];memset(z,1,sizeof(z));CHECK(pread(f,z,sizeof(z),0)==sizeof(z));for(size_t i=0;i<sizeof(z);i++)CHECK(!z[i]);
 CHECK(pwrite(f,"preserved",9,p+3)==9);CHECK(!fallocate(f,0,0,4*p));state(f,4*p,4*p/512);
 char *map=mmap(NULL,4*p,PROT_READ|PROT_WRITE,MAP_SHARED,f,0);CHECK(map!=MAP_FAILED);CHECK(!memcmp(map+p+3,"preserved",9));
 if(quota){FAIL(ENOSPC,fallocate(f,0,0,5*p));state(f,4*p,4*p/512);}
 CHECK(!fallocate(f,1,0,4*p));map[3*p]='Z';CHECK(pread(f,z,1,3*p)==1&&z[0]=='Z');
 CHECK(!fallocate(f,3,2*p,p));state(f,4*p,3*p/512);CHECK(map[2*p]==0);CHECK(!fallocate(f,0,2*p,p));state(f,4*p,4*p/512);map[2*p]='M';CHECK(pread(f,z,1,2*p)==1&&z[0]=='M');CHECK(!munmap(map,4*p));
 CHECK(!ftruncate(f,0));CHECK(!fallocate(f,1,0,4*p));state(f,0,4*p/512);
 CHECK(!ftruncate(f,p));state(f,p,4*p/512);CHECK(!ftruncate(f,p));state(f,p,p/512);
 CHECK(!fallocate(f,1,3*p,p));state(f,p,2*p/512);CHECK(!fallocate(f,3,3*p,p));state(f,p,p/512);
 CHECK(!ftruncate(f,0));
 struct rlimit old,limit;CHECK(!getrlimit(RLIMIT_FSIZE,&old));limit=old;limit.rlim_cur=p;
 CHECK(signal(SIGXFSZ,SIG_IGN)!=SIG_ERR);CHECK(!setrlimit(RLIMIT_FSIZE,&limit));
 FAIL(EFBIG,fallocate(f,0,0,2*p));state(f,0,0);
 FAIL(EFBIG,fallocate(f,1,0,2*p));state(f,0,0);CHECK(!setrlimit(RLIMIT_FSIZE,&old));CHECK(!ftruncate(f,0));
 if(quota){CHECK(!fallocate(f,1,2*p,p));FAIL(ENOSPC,fallocate(f,0,0,5*p));state(f,0,p/512);CHECK(!fallocate(f,0,0,4*p));state(f,4*p,4*p/512);}
 CHECK(!close(f));int ro=open(name,O_RDONLY);CHECK(ro>=0);FAIL(EBADF,fallocate(ro,0,0,p));CHECK(!close(ro)&&!unlink(name));puts("FALLOC CONTRACT PASS");
}
int main(int argc,char **argv)
{
 setbuf(stdout,NULL);
 if(argc==2)contract(argv[1],0);
 else {long p=sysconf(_SC_PAGESIZE);char options[80];snprintf(options,sizeof(options),"size=%ld",4*p);CHECK(!mkdir("/reserve",0777));CHECK(!mount("tmpfs","/reserve","tmpfs",0,options));contract("/reserve/file",1);CHECK(!umount("/reserve")&&!rmdir("/reserve"));}
 puts("FALLOC ALL PASS");return 0;
}
