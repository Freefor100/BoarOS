#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#define CHECK(x) do {if(!(x)){printf("CORE FAIL line=%d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
static char *memory;static size_t page;static pid_t crashed_pid;
static int ready[2],hold[2],identity[2];
static int thread_mode;static pid_t crashed_thread;
static volatile uint64_t spinning;
static void *other_thread(void *unused)
{ (void)unused;char c;pid_t tid=(pid_t)syscall(SYS_gettid);
 CHECK(write(identity[1],&tid,sizeof(tid))==sizeof(tid));
 CHECK(write(ready[1],"R",1)==1);
 if(thread_mode==7)raise(SIGABRT);
 if(thread_mode==6)for(;;)spinning++;
 CHECK(read(hold[0],&c,1)==1);return NULL; }
static int crash(int mode)
{
 int threaded=mode==3||mode==6||mode==7;
 if(threaded)CHECK(!pipe(identity));
 pid_t pid=fork();CHECK(pid>=0);
 if(!pid){
  struct rlimit limit={mode==1?0:mode==5?page:32*1024*1024,RLIM_INFINITY};CHECK(!setrlimit(RLIMIT_CORE,&limit));
  CHECK(!prctl(PR_SET_DUMPABLE,mode==2?0:1));
  if(mode==4){struct rlimit fsize={0,RLIM_INFINITY};CHECK(!setrlimit(RLIMIT_FSIZE,&fsize));}
  if(threaded){thread_mode=mode;pthread_t t;CHECK(!pipe(ready)&&!pipe(hold));CHECK(!pthread_create(&t,NULL,other_thread,NULL));char c;CHECK(read(ready[0],&c,1)==1);}
  if(mode==7)for(;;)pause();
  raise(SIGABRT);_exit(99);
 }
 crashed_pid=pid;
 int status;CHECK(waitpid(pid,&status,0)==pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
 if(threaded){CHECK(read(identity[0],&crashed_thread,sizeof(crashed_thread))==sizeof(crashed_thread));CHECK(!close(identity[0])&&!close(identity[1]));}
 return status;
}
static void inspect(void)
{
 int fd=open("core",O_RDONLY);CHECK(fd>=0);struct stat st;CHECK(!fstat(fd,&st));CHECK((st.st_mode&0777)==0600);
 Elf64_Ehdr e;CHECK(pread(fd,&e,sizeof(e),0)==sizeof(e));CHECK(!memcmp(e.e_ident,ELFMAG,4)&&e.e_type==ET_CORE&&e.e_phentsize==56);
 int note=0,mapping=0,hidden=0;
 for(unsigned i=0;i<e.e_phnum;i++){
  Elf64_Phdr p;CHECK(pread(fd,&p,sizeof(p),e.e_phoff+i*sizeof(p))==sizeof(p));
  if(p.p_type==PT_NOTE){Elf64_Nhdr n;CHECK(pread(fd,&n,sizeof(n),p.p_offset)==sizeof(n));CHECK(n.n_type==NT_PRSTATUS);unsigned char desc[512];CHECK(n.n_descsz<=sizeof(desc));CHECK(pread(fd,desc,n.n_descsz,p.p_offset+12+((n.n_namesz+3)&~3U))==n.n_descsz);
   int32_t tid;uint16_t sig;memcpy(&tid,desc+32,4);memcpy(&sig,desc+12,2);CHECK(tid==crashed_pid&&sig==SIGABRT);
#if defined(__riscv)
   uint64_t pc,sp;memcpy(&pc,desc+112,8);memcpy(&sp,desc+112+2*8,8);CHECK(pc&&sp);
#elif defined(__loongarch__)
   uint64_t pc,sp;memcpy(&pc,desc+112+33*8,8);memcpy(&sp,desc+112+3*8,8);CHECK(pc&&sp);
#endif
   note=1;}
  if(p.p_type==PT_LOAD && (uintptr_t)memory>=p.p_vaddr&&(uintptr_t)memory<p.p_vaddr+p.p_memsz){
   char data[16];CHECK(p.p_flags==PF_R);CHECK(pread(fd,data,sizeof(data),p.p_offset+(uintptr_t)memory-p.p_vaddr)==sizeof(data));CHECK(!memcmp(data,"core marker 123",16));mapping=1;
  }
  if(p.p_type==PT_LOAD && (uintptr_t)(memory+page)>=p.p_vaddr&&(uintptr_t)(memory+page)<p.p_vaddr+p.p_memsz){CHECK(p.p_filesz==0);hidden=1;}
 }
 CHECK(note&&mapping&&hidden);CHECK(!close(fd));
}
static int shared_mm_crash(void *unused)
{ (void)unused;syscall(SYS_tgkill,syscall(SYS_getpid),syscall(SYS_gettid),SIGABRT);_exit(99); }
int main(void)
{
 setbuf(stdout,NULL);page=(size_t)sysconf(_SC_PAGESIZE);CHECK(!mkdir("core-test",0700));CHECK(!chdir("core-test"));
 memory=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(memory!=MAP_FAILED);
 memcpy(memory,"core marker 123",16);CHECK(!mprotect(memory,page,PROT_READ));CHECK(!madvise(memory+page,page,MADV_DONTDUMP));
 CHECK(WCOREDUMP(crash(0)));inspect();puts("CORE CASE image PASS");
 CHECK(!unlink("core"));CHECK(!WCOREDUMP(crash(1)));CHECK(access("core",F_OK)<0);puts("CORE CASE limit PASS");
 CHECK(!WCOREDUMP(crash(2)));CHECK(access("core",F_OK)<0);puts("CORE CASE dumpable PASS");
 CHECK(!symlink("target","core"));CHECK(!WCOREDUMP(crash(0)));CHECK(access("target",F_OK)<0);CHECK(!unlink("core"));puts("CORE CASE symlink PASS");
 int fd=open("core",O_CREAT|O_WRONLY,0600);CHECK(fd>=0);CHECK(write(fd,"retain",6)==6);CHECK(!close(fd));CHECK(!link("core","alias"));
 CHECK(!WCOREDUMP(crash(0)));struct stat st;CHECK(!stat("core",&st)&&st.st_size==6);CHECK(!unlink("alias"));puts("CORE CASE hardlink PASS");
 CHECK(WCOREDUMP(crash(0)));inspect();puts("CORE CASE replace PASS");
 CHECK(!unlink("core"));
 /* Each live TID must have a distinct register note, including when the
  * nonleader is the crashing task or its sibling was running on another CPU. */
 for(int mode=3;mode<=7;mode++){if(mode==4||mode==5)continue;
  CHECK(WCOREDUMP(crash(mode)));
  int fd2=open("core",O_RDONLY);CHECK(fd2>=0);Elf64_Ehdr e;CHECK(read(fd2,&e,sizeof(e))==sizeof(e));unsigned notes=0,seen=0;
  for(unsigned i=0;i<e.e_phnum;i++){Elf64_Phdr p;CHECK(pread(fd2,&p,sizeof(p),e.e_phoff+i*sizeof(p))==sizeof(p));
   if(p.p_type==PT_NOTE)for(uint64_t at=p.p_offset;at<p.p_offset+p.p_filesz;){Elf64_Nhdr n;CHECK(pread(fd2,&n,sizeof(n),at)==sizeof(n));
    if(n.n_type==NT_PRSTATUS){unsigned char desc[512];CHECK(n.n_descsz<=sizeof(desc));CHECK(pread(fd2,desc,n.n_descsz,at+12+((n.n_namesz+3)&~3U))==n.n_descsz);
     int32_t tid;uint16_t sig;memcpy(&tid,desc+32,4);memcpy(&sig,desc+12,2);CHECK(sig==SIGABRT);
     unsigned bit=tid==crashed_pid?1:tid==crashed_thread?2:0;CHECK(bit&&!(seen&bit));seen|=bit;notes++;
#if defined(__riscv)
     uint64_t pc,sp;memcpy(&pc,desc+112,8);memcpy(&sp,desc+128,8);CHECK(pc&&sp);
#elif defined(__loongarch__)
     uint64_t pc,sp;memcpy(&pc,desc+112+33*8,8);memcpy(&sp,desc+112+3*8,8);CHECK(pc&&sp);
#endif
    }
    at+=12+((n.n_namesz+3)&~3U)+((n.n_descsz+3)&~3U);}}
  CHECK(notes==2&&seen==3);CHECK(!close(fd2));
  if(mode==7)CHECK(!rename("core","core-threads"));else CHECK(!unlink("core"));
 }
 CHECK(!symlink("target","core"));CHECK(!WCOREDUMP(crash(3)));CHECK(access("target",F_OK)<0);CHECK(!unlink("core"));
 struct rlimit shared_limit={32*1024*1024,RLIM_INFINITY};CHECK(!setrlimit(RLIMIT_CORE,&shared_limit));
 void *stack=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(stack!=MAP_FAILED);
 pid_t shared=clone(shared_mm_crash,(char *)stack+65536,CLONE_VM|CLONE_VFORK|SIGCHLD,NULL);CHECK(shared>0);
 int shared_status;CHECK(waitpid(shared,&shared_status,0)==shared&&WIFSIGNALED(shared_status)&&WTERMSIG(shared_status)==SIGABRT);
 CHECK(!WCOREDUMP(shared_status)&&access("core",F_OK)<0);CHECK(!munmap(stack,65536));
 puts("CORE CASE threads-blocked-running-nonleader-failure PASS");
 CHECK(!WCOREDUMP(crash(4)));CHECK(access("core",F_OK)<0);puts("CORE CASE fsize PASS");
 fd=open("core",O_CREAT|O_WRONLY,0644);CHECK(fd>=0);CHECK(write(fd,"retain",6)==6);CHECK(!close(fd));
 CHECK(!WCOREDUMP(crash(0)));CHECK(!stat("core",&st)&&st.st_size==6);CHECK(!unlink("core"));puts("CORE CASE unsafe-mode PASS");
 CHECK(WCOREDUMP(crash(5)));CHECK(!stat("core",&st)&&st.st_size<=(off_t)page);CHECK(!unlink("core"));puts("CORE CASE bounded-prefix PASS");
 CHECK(WCOREDUMP(crash(0)));inspect();
 CHECK(!munmap(memory,page*2));puts("CORE END 10");return 0;
}
