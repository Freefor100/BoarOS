#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    char mode[16],platform[16];
    FILE *profile=fopen("/work/profile","r");
    if(!profile || fscanf(profile,"%15s %15s",mode,platform)!=2) {
        perror("BPF init profile");return 1;
    }
    fclose(profile);
    /* Linux devtmpfs can cover the image's /dev before init. LLVM reopens its
     * real output descriptors through the conventional /dev/fd alias. */
    if(symlink("/proc/self/fd","/dev/fd") && errno!=EEXIST) {
        perror("BPF init /dev/fd");return 1;
    }
    int status=1;
    int child=fork();
    if(!child){
        char *const argv[]={"/bin/sh","/work/run.sh",mode,NULL};
        execv(argv[0],argv);
        perror("BPF init exec");_exit(127);
    }
    int passed=child>0 && waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status);
    if(passed)puts("BPF GUEST INIT PASS");
    else fprintf(stderr,"BPF GUEST INIT FAIL status=%d errno=%d\n",status,errno);
    sync();
    if(!strcmp(platform,"linux"))reboot(RB_POWER_OFF);
    return !passed;
}
