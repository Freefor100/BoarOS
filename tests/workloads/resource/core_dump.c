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
#define CHECK(x) do {if(!(x)){printf("CORE FAIL line=%d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
static char *memory;static size_t page;static pid_t crashed_pid;
static int ready[2],hold[2];
static void *other_thread(void *unused)
{ (void)unused;char c;CHECK(write(ready[1],"R",1)==1);CHECK(read(hold[0],&c,1)==1);return NULL; }
static int crash(int mode)
{
 pid_t pid=fork();CHECK(pid>=0);
 if(!pid){
  struct rlimit limit={mode==1?0:mode==5?page:32*1024*1024,RLIM_INFINITY};CHECK(!setrlimit(RLIMIT_CORE,&limit));
  CHECK(!prctl(PR_SET_DUMPABLE,mode==2?0:1));
  if(mode==4){struct rlimit fsize={0,RLIM_INFINITY};CHECK(!setrlimit(RLIMIT_FSIZE,&fsize));}
  if(mode==3){pthread_t t;CHECK(!pipe(ready)&&!pipe(hold));CHECK(!pthread_create(&t,NULL,other_thread,NULL));char c;CHECK(read(ready[0],&c,1)==1);}
  raise(SIGABRT);_exit(99);
 }
 crashed_pid=pid;
 int status;CHECK(waitpid(pid,&status,0)==pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);return status;
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
 int threaded=crash(3);
 /* A staged kernel may decline the whole dump, but must not publish a
  * misleading single-thread core while another execution context is live. */
 if(!WCOREDUMP(threaded)) CHECK(access("core",F_OK)<0);
 else {
  int fd2=open("core",O_RDONLY);CHECK(fd2>=0);Elf64_Ehdr e;CHECK(read(fd2,&e,sizeof(e))==sizeof(e));unsigned notes=0;
  for(unsigned i=0;i<e.e_phnum;i++){Elf64_Phdr p;CHECK(pread(fd2,&p,sizeof(p),e.e_phoff+i*sizeof(p))==sizeof(p));
   if(p.p_type==PT_NOTE)for(uint64_t at=p.p_offset;at<p.p_offset+p.p_filesz;){Elf64_Nhdr n;CHECK(pread(fd2,&n,sizeof(n),at)==sizeof(n));if(n.n_type==NT_PRSTATUS)notes++;at+=12+((n.n_namesz+3)&~3U)+((n.n_descsz+3)&~3U);}}
  CHECK(notes>=2);CHECK(!close(fd2));CHECK(!unlink("core"));
 }
 puts("CORE CASE threads-all-or-none PASS");
 CHECK(!WCOREDUMP(crash(4)));CHECK(access("core",F_OK)<0);puts("CORE CASE fsize PASS");
 fd=open("core",O_CREAT|O_WRONLY,0644);CHECK(fd>=0);CHECK(write(fd,"retain",6)==6);CHECK(!close(fd));
 CHECK(!WCOREDUMP(crash(0)));CHECK(!stat("core",&st)&&st.st_size==6);CHECK(!unlink("core"));puts("CORE CASE unsafe-mode PASS");
 CHECK(WCOREDUMP(crash(5)));CHECK(!stat("core",&st)&&st.st_size<=(off_t)page);CHECK(!unlink("core"));puts("CORE CASE bounded-prefix PASS");
 CHECK(WCOREDUMP(crash(0)));inspect();
 CHECK(!munmap(memory,page*2));puts("CORE END 10");return 0;
}
