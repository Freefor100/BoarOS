/* PID1 only coordinates existing fixed-work consumers and owns /proc. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(v) do { if(!(v)) { dprintf(2,"SMP-SYSTEM FAIL line=%d %s errno=%d\n",__LINE__,#v,errno);_exit(1); } } while(0)
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);unsigned cpus=0;char profile[16]="formal";
    FILE *config=fopen("/smp-scale-cost","r");CHECK(config);
    CHECK(fscanf(config,"%u %15s",&cpus,profile)==2&&!fclose(config));
    CHECK((cpus==1||cpus==2||cpus==4||cpus==8)&&(!strcmp(profile,"formal")||!strcmp(profile,"smoke")));
    CHECK(getpid()==1);struct utsname uts;CHECK(!uname(&uts));
    int linux_pid1=!strcmp(uts.sysname,"Linux")&&!strstr(uts.release,"boaros");
    CHECK(!mkdir("/proc",0755)||errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,NULL));
    const char *programs[]={"/scale","/pipe","/event","/network","/mm"};
    printf("SMP-SYSTEM BEGIN version=1 cpus=%u profile=%s consumers=5\n",cpus,profile);
    for(unsigned i=0;i<sizeof(programs)/sizeof(*programs);i++) {
        pid_t child=fork();CHECK(child>=0);
        if(!child) {execl(programs[i],programs[i],(char *)NULL);_exit(1);}
        int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    }
    CHECK(!umount("/proc"));
    printf("SMP-SYSTEM END consumers=5 windows=27 cpus=%u profile=%s\n",cpus,profile);
    if(linux_pid1) {sync();CHECK(!reboot(RB_POWER_OFF));}return 0;
}
