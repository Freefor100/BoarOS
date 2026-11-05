#ifndef COST_WORKLOAD_COMMON_H
#define COST_WORKLOAD_COMMON_H
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"cost workload failed line %d errno %d\n",__LINE__,errno); exit(1); } } while (0)
static int cost_control=-1;
static unsigned cost_epoch;
static struct timespec window_start;
static void cost_init(void)
{
    mkdir("/proc",0755); CHECK(mount("proc","/proc","proc",0,0)==0);
    cost_control=open("/proc/boaros_cost_control",O_WRONLY);
    CHECK(cost_control>=0 || errno==ENOENT);
}
static void cost_begin(void)
{
    if (cost_control>=0) CHECK(write(cost_control,"begin\n",6)==6);
    cost_epoch++;
    CHECK(clock_gettime(CLOCK_MONOTONIC,&window_start)==0);
}
static void cost_end(const char *name, const char *kind, unsigned calls, unsigned long long requested, unsigned long long accepted)
{
    struct timespec stop; CHECK(clock_gettime(CLOCK_MONOTONIC,&stop)==0);
    if (cost_control>=0) {
#ifdef COST_END_WAIT_ASYNC
        unsigned retries=0;
        struct timespec closed=stop;
        while (write(cost_control,"end\n",4)!=4) {
            CHECK(errno==EBUSY);
            CHECK(clock_gettime(CLOCK_MONOTONIC,&closed)==0);
            CHECK(closed.tv_sec-stop.tv_sec<10);
            struct timespec delay={0,1000000};CHECK(nanosleep(&delay,0)==0);
            retries++;
        }
        CHECK(clock_gettime(CLOCK_MONOTONIC,&closed)==0);
        unsigned long long closing=(closed.tv_sec-stop.tv_sec)*1000000000ULL+closed.tv_nsec-stop.tv_nsec;
        printf("COST CLOSING %s %llu %u\n",name,closing,retries);
#else
        CHECK(write(cost_control,"end\n",4)==4);
#endif
    }
    unsigned long long ns=(stop.tv_sec-window_start.tv_sec)*1000000000ULL+stop.tv_nsec-window_start.tv_nsec;
    printf("COST RESULT %s %llu\n",name,ns);
    if (kind) printf("COST EXPECT %s %s %u %llu %llu\n",name,kind,calls,requested,accepted);
    if (cost_control>=0) {
        int fd=open("/proc/boaros_cost",O_RDONLY); CHECK(fd>=0);
        printf("COST SNAPSHOT %s %u\n",name,cost_epoch); fflush(stdout);
        char buffer[4096]; ssize_t n;
        while ((n=read(fd,buffer,sizeof(buffer)))>0) CHECK(fwrite(buffer,1,(size_t)n,stdout)==(size_t)n);
        CHECK(n==0); CHECK(close(fd)==0); puts("COST END");
    }
    fflush(stdout);
}
#endif
