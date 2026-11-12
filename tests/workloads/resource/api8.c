#define _GNU_SOURCE
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"API8 FAIL line=%d %s errno=%d\n",__LINE__,#x,errno); return 1; } } while (0)
static int directory(const char *path)
{ return !mkdir(path,0777) || errno==EEXIST; }
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    CHECK(directory("/proc")&&directory("/tmp")&&directory("/dev/mqueue"));
    int proc=!mount("proc","/proc","proc",0,NULL);CHECK(proc || errno==EBUSY);
    int mq=!mount("mqueue","/dev/mqueue","mqueue",0,NULL);CHECK(mq || errno==EBUSY);
    int fat=access("/with-vfat",F_OK)==0;
    if(fat) {
        CHECK(directory("/fat") && !mknod("/dev/vdb",S_IFBLK|0600,makedev(252,16)));
        CHECK(!mount("/dev/vdb","/fat","vfat",0,NULL));
        CHECK(!setenv("COPY_RANGE_VFAT","/fat",1));
    }
    const char *defaults[]={"/fd_events","/flock","/mmsg","/copy_range",
#ifdef __riscv
        "/icache",
#endif
        "/sched_query",
    };
    const char *programs[16];char paths[16][48];
    unsigned count=sizeof(defaults)/sizeof(defaults[0]);
    for(unsigned i=0;i<count;i++)programs[i]=defaults[i];
    FILE *selection=fopen("/api8-programs","r");
    if(selection) {
        count=0;
        while(count<16 && fgets(paths[count],sizeof(paths[count]),selection)) {
            size_t length=strcspn(paths[count],"\n");
            CHECK(paths[count][0]=='/' && paths[count][length]=='\n');
            paths[count][length]=0;programs[count]=paths[count];count++;
        }
        CHECK(count && feof(selection) && !fclose(selection));
    }
    int network=0;
    for(unsigned i=0;i<count;i++)network|=!strcmp(programs[i],"/mmsg");
    if(network) {
    int fd=socket(AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);
    struct ifreq interface={0};strcpy(interface.ifr_name,"lo");
    CHECK(!ioctl(fd,SIOCGIFFLAGS,&interface));interface.ifr_flags|=IFF_UP;
    CHECK(!ioctl(fd,SIOCSIFFLAGS,&interface)&&!close(fd));
    }

    for(unsigned i=0;i<count;i++) {
        printf("API8 BEGIN %s\n",programs[i]);
        pid_t child=fork();CHECK(child>=0);
        if (!child) { execl(programs[i],programs[i],(char *)NULL);perror(programs[i]);_exit(127); }
        int status;CHECK(waitpid(child,&status,0)==child);
        if (!WIFEXITED(status)||WEXITSTATUS(status)) {
            fprintf(stderr,"API8 FAIL child=%s status=%d\n",programs[i],status);return 1;
        }
        printf("API8 END %s status=0\n",programs[i]);
    }
    CHECK((!mq||!umount("/dev/mqueue"))&&(!proc||!umount("/proc")));
    CHECK(!fat || !umount("/fat"));
    printf("API8 PASS programs=%u\n",count);return 0;
}
