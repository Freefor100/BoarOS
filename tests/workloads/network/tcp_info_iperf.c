#define _GNU_SOURCE
#include <unistd.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <stdio.h>
#include <stdlib.h>

/* A bounded consumer check, not a performance measurement. */
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    alarm(30);
    pid_t server=fork();
    if(server<0)return 1;
    if(!server) {
        execl("/iperf3","iperf3","-s","-1","-p","5021","-J",NULL);
        _exit(127);
    }
    sleep(1);
    pid_t client=fork();
    if(client<0)return 2;
    if(!client) {
        execl("/iperf3","iperf3","-c","127.0.0.1","-p","5021",
              "-n","32768","-l","4096","-J",NULL);
        _exit(127);
    }
    int cs=-1,ss=-1;
    if(waitpid(client,&cs,0)!=client || waitpid(server,&ss,0)!=server || cs || ss) {
        printf("IPERF FAIL client=%d server=%d\n",cs,ss);
        return 3;
    }
    puts("IPERF TCPINFO PASS");
    reboot(RB_POWER_OFF);
    return 0;
}
