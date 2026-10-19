#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(v) do {if (!(v)) {dprintf(2,"ACCOUNT FAIL line=%d errno=%d\n",__LINE__,errno);exit(1);}} while(0)
static uint64_t now(void)
{
    struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000U+t.tv_nsec;
}
static void result(const char *name,uint64_t start,uint64_t check)
{printf("ACCOUNT COST %s ns=%llu check=%llu\n",name,(unsigned long long)(now()-start),(unsigned long long)check);fflush(stdout);}
static void sqlite(const char *database,const char *sql,const char *expected)
{
    int output[2];CHECK(!pipe(output));pid_t child=fork();CHECK(child>=0);
    if (!child) {
        CHECK(!close(output[0]) && dup2(output[1],1)==1 && !close(output[1]));
        execl("/sqlite3","sqlite3",database,sql,(char *)0);_exit(97);
    }
    CHECK(!close(output[1]));char buffer[128];size_t count=0;
    for (;;) {
        ssize_t n=read(output[0],buffer+count,sizeof(buffer)-1-count);CHECK(n>=0);
        if (!n) break;
        count+=(size_t)n;CHECK(count<sizeof(buffer)-1);
    }
    CHECK(!close(output[0]));int status;CHECK(waitpid(child,&status,0)==child && status==0);
    buffer[count]=0;CHECK(!strcmp(buffer,expected));
}
int main(void)
{
    uint64_t start=now(),value=1;
    for (unsigned i=0;i<200000000;i++) {value=(value<<3)^(value>>7)^17;__asm__ volatile("" : "+r"(value));}
    CHECK(value==UINT64_C(0x36de7ac67e71a943));
    result("compute",start,value);
    CHECK(!mkdir("/tmp",01777) || errno==EEXIST);
    CHECK(!mount("tmpfs","/tmp","tmpfs",0,0));
    unsigned char bytes[64],out[64];for (unsigned i=0;i<sizeof(bytes);i++) bytes[i]=(unsigned char)(i+1);
    int fd=open("/tmp/small-write",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
    start=now();
    for (unsigned i=0;i<10000;i++) CHECK(write(fd,bytes,sizeof(bytes))==sizeof(bytes));
    CHECK(lseek(fd,0,SEEK_SET)==0);
    for (unsigned i=0;i<10000;i++) CHECK(read(fd,out,sizeof(out))==sizeof(out) && !memcmp(bytes,out,sizeof(out)));
    CHECK(!close(fd));result("tmpfs-small-io",start,640000);
    start=now();
    sqlite(":memory:","WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<50000) SELECT sum(x) FROM n;","1250025000\n");
    result("sqlite-memory",start,1250025000);
    char sql[8192];size_t used=(size_t)snprintf(sql,sizeof(sql),
        "PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL;CREATE TABLE t(id INTEGER PRIMARY KEY,v INTEGER);"
        "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<5000) INSERT INTO t SELECT x,x FROM n;");
    for (unsigned i=0;i<40;i++) used+=(size_t)snprintf(sql+used,sizeof(sql)-used,"BEGIN;UPDATE t SET v=v+1 WHERE id=%u;COMMIT;",i+1);
    CHECK(used+80<sizeof(sql));strcpy(sql+used,"PRAGMA integrity_check;SELECT count(*),sum(v) FROM t;");
    start=now();sqlite("/account.db",sql,"delete\nok\n5000|12502540\n");result("sqlite-commit",start,12502540);
    CHECK(!unlink("/account.db"));sync();puts("ACCOUNT END 4");fflush(stdout);return 42;
}
