#define COST_END_WAIT_ASYNC 1
#include "common.h"
#include <sys/wait.h>
#define BYTES (8U*1024U*1024U)
struct result { unsigned id; unsigned long long start, finish, bytes, checksum; };
static unsigned long long now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (unsigned long long)t.tv_sec*1000000000+t.tv_nsec;}
static void exact(int fd,void *p,size_t n){size_t done=0;while(done<n){ssize_t r=read(fd,(char *)p+done,n-done);CHECK(r>0);done+=r;}}
#ifdef COST_READERS_EMBED
static void cost_readers(void)
#else
int main(void)
#endif
{
#ifndef COST_READERS_EMBED
 cost_init();
#endif
 for(unsigned separate=0;separate<2;separate++)for(unsigned hot=0;hot<2;hot++){
  char name[64], path[40], buffer[1024];
  snprintf(name,sizeof(name),"readers-%u-%u",separate,hot);
  if(hot)for(unsigned i=0;i<(separate?4U:1U);i++){
   snprintf(path,sizeof(path),"/reader-%u-%u",separate,i);int fd=open(path,O_RDONLY);CHECK(fd>=0);
   for(unsigned j=0;j<BYTES/sizeof(buffer);j++){exact(fd,buffer,sizeof(buffer));}
   CHECK(!close(fd));
  }
  int ready[2], report[2], gates[4][2];pid_t pids[4];CHECK(!pipe(ready)&&!pipe(report));
  for(unsigned i=0;i<4;i++)CHECK(!pipe(gates[i]));
  for(unsigned i=0;i<4;i++){
   pids[i]=fork();CHECK(pids[i]>=0);
   if(!pids[i]){
    snprintf(path,sizeof(path),"/reader-%u-%u",separate,separate?i:0);int fd=open(path,O_RDONLY);CHECK(fd>=0);
    CHECK(write(ready[1],"r",1)==1);char c;exact(gates[i][0],&c,1);
    struct result value={.id=i,.start=now()};
    for(unsigned j=0;j<BYTES/sizeof(buffer);j++){
     exact(fd,buffer,sizeof(buffer));value.bytes+=sizeof(buffer);
     for(unsigned k=0;k<sizeof(buffer);k++)value.checksum+=(unsigned char)buffer[k];
    }
    value.finish=now();CHECK(value.bytes==BYTES && value.checksum==(unsigned long long)BYTES*90);
    CHECK(!close(fd));CHECK(write(report[1],&value,sizeof(value))==sizeof(value));_exit(0);
   }
  }
  exact(ready[0],buffer,4);cost_begin();unsigned long long release=now();
  for(unsigned i=0;i<4;i++)CHECK(write(gates[i][1],"g",1)==1);
  struct result values[4];unsigned mask=0;
  for(unsigned i=0;i<4;i++){exact(report[0],&values[i],sizeof(values[i]));CHECK(values[i].id<4 && !(mask&(1U<<values[i].id)));mask|=1U<<values[i].id;}
  for(unsigned i=0;i<4;i++){int status;CHECK(waitpid(pids[i],&status,0)==pids[i]&&WIFEXITED(status)&&!WEXITSTATUS(status));}
  cost_end(name,NULL,0,0,0);
  for(unsigned i=0;i<4;i++)printf("COST READER %s %u %llu %llu %llu %llu %llu\n",name,values[i].id,release,values[i].start,values[i].finish,values[i].bytes,values[i].checksum);
  CHECK(!close(ready[0])&&!close(ready[1])&&!close(report[0])&&!close(report[1]));
  for(unsigned i=0;i<4;i++)CHECK(!close(gates[i][0])&&!close(gates[i][1]));
 }
 puts("COST PASS readers");
#ifndef COST_READERS_EMBED
 return 0;
#endif
}
