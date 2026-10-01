#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"network-consumers:%d: %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while(0)
static long long now_ms(void)
{ struct timespec t;CHECK(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (long long)t.tv_sec*1000+t.tv_nsec/1000000; }
static void pause_ms(long ms)
{ struct timespec t={ms/1000,(ms%1000)*1000000};while(nanosleep(&t,&t)<0 && errno==EINTR){} }
static int cost_control=-1;
static unsigned cost_epoch;
static void protocol_snapshot(const char *name, const char *phase)
{
    if(cost_control<0)return;
    FILE *stream=fopen("/proc/boaros_net_stats","r");CHECK(stream!=NULL);
    printf("NETWORK PROTOCOL name=%s phase=%s\n",name,phase);
    char line[128];while(fgets(line,sizeof(line),stream))fputs(line,stdout);
    CHECK(!ferror(stream) && fclose(stream)==0);puts("NETWORK PROTOCOL END");
}
static void begin_window(const char *name)
{
    protocol_snapshot(name,"before");
    if(cost_control>=0){CHECK(write(cost_control,"begin\n",6)==6);cost_epoch++;}
}
static void end_window(const char *name)
{
    if(cost_control<0)return;
    CHECK(write(cost_control,"end\n",4)==4);
    printf("COST SNAPSHOT %s %u\n",name,cost_epoch);
    FILE *stream=fopen("/proc/boaros_cost","r");CHECK(stream!=NULL);
    char buffer[4096];size_t n;
    while((n=fread(buffer,1,sizeof(buffer),stream)))CHECK(fwrite(buffer,1,n,stdout)==n);
    CHECK(!ferror(stream) && fclose(stream)==0);puts("COST END");
    protocol_snapshot(name,"after");
}
static void drain_protocol(void)
{
    if(cost_control<0)return;
    long long start=now_ms();unsigned active=0,listen=0,segments=0,udp=0,used=0;
    do {
        FILE *stream=fopen("/proc/boaros_net_stats","r");CHECK(stream!=NULL);
        char key[40];unsigned long long value;
        while(fscanf(stream,"%39[^=]=%llu\n",key,&value)==2){
            if(!strcmp(key,"active_pcbs"))active=(unsigned)value;
            if(!strcmp(key,"listen_pcbs"))listen=(unsigned)value;
            if(!strcmp(key,"segments"))segments=(unsigned)value;
            if(!strcmp(key,"udp_pcbs"))udp=(unsigned)value;
            if(!strcmp(key,"heap_used"))used=(unsigned)value;
        }
        CHECK(fclose(stream)==0 && now_ms()-start<130000);
        if(active||listen||segments||udp||used)pause_ms(100);
    }while(active||listen||segments||udp||used);
    printf("NETWORK PROTOCOL DRAIN elapsed_ms=%lld active=0 listen=0 segments=0 udp=0 heap=0\n",now_ms()-start);
}
static pid_t spawn(char *const args[], const char *output)
{
    pid_t child=fork();CHECK(child>=0);
    if(!child){
        if(output){int fd=open(output,O_CREAT|O_TRUNC|O_WRONLY,0600);CHECK(fd>=0);
            CHECK(dup2(fd,1)==1 && dup2(fd,2)==2 && close(fd)==0);}
        execv(args[0],args);perror(args[0]);_exit(127);
    }
    return child;
}
static void show_output(const char *path)
{
    FILE *stream=fopen(path,"r");CHECK(stream!=NULL);
    char buffer[4096];size_t n;
    while((n=fread(buffer,1,sizeof(buffer),stream))!=0)CHECK(fwrite(buffer,1,n,stdout)==n);
    CHECK(!ferror(stream) && fclose(stream)==0 && unlink(path)==0);
}
static int wait_command(pid_t child, const char *name, long budget, const char *output)
{
    int status=0,timeout=0;long long start=now_ms(),deadline=start+budget;
    for(;;){pid_t result=waitpid(child,&status,WNOHANG);CHECK(result>=0);
        if(result==child)break;
        if(now_ms()>=deadline && !timeout){timeout=1;CHECK(kill(child,SIGKILL)==0);}
        pause_ms(10);
    }
    long long elapsed=now_ms()-start;
    if(output)show_output(output);
    printf("NETWORK COMMAND END name=%s wait=%d timeout=%d elapsed_ms=%lld\n",name,status,timeout,elapsed);
    return !timeout && WIFEXITED(status) && WEXITSTATUS(status)==0;
}
static void cleanup(void)
{
    /* PID 1 owns these guest-only descendants; terminate daemonized servers too. */
    DIR *proc=opendir("/proc");CHECK(proc!=NULL);struct dirent *entry;
    while((entry=readdir(proc))!=NULL){char *end;long pid=strtol(entry->d_name,&end,10);
        if(*end==0 && pid>1 && pid!=getpid()) { if(kill((pid_t)pid,SIGKILL)<0)CHECK(errno==ESRCH); }}
    CHECK(closedir(proc)==0);
    long long deadline=now_ms()+5000;
    for(;;){int status;pid_t pid=waitpid(-1,&status,WNOHANG);
        if(pid<0){CHECK(errno==ECHILD);break;}
        CHECK(now_ms()<deadline);if(pid==0)pause_ms(10);
    }
    puts("NETWORK CLEANUP descendants=0");
}
static void setup_loopback(const char *platform)
{
    int fd=socket(AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);
    struct ifreq interface={.ifr_name="lo"};CHECK(ioctl(fd,SIOCGIFFLAGS,&interface)==0);
    interface.ifr_flags|=IFF_UP;CHECK(ioctl(fd,SIOCSIFFLAGS,&interface)==0);
    if(!strcmp(platform,"linux")) {
        /* glibc AI_ADDRCONFIG ignores exactly 127.0.0.1: keep a local-only alias. */
        struct ifreq alias={.ifr_name="lo:1"};
        struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={htonl(0x7f000002)}};
        memcpy(&alias.ifr_addr,&address,sizeof(address));CHECK(ioctl(fd,SIOCSIFADDR,&alias)==0);
        printf("NETWORK REFERENCE loopback_alias=127.0.0.2 AI_ADDRCONFIG=IPv4\n");
    }
    CHECK(close(fd)==0);
    if(mount("proc","/proc","proc",0,NULL)<0)CHECK(errno==EBUSY);
}
static void ready_server(unsigned port, int iperf)
{
    long long deadline=now_ms()+5000;
    if(iperf) {
        /* 探测连接会让 iperf 关闭/重建 listener；用 listen 后的真实输出握手。 */
        for(;;){
            FILE *stream=fopen("/tmp/network-server.out","r");int ready=0;
            if(stream){
                char line[256];
                while(fgets(line,sizeof(line),stream)) {
                    if(strstr(line,"Server listening on 5001"))ready=1;
                }
                CHECK(fclose(stream)==0);
            }
            if(ready){printf("NETWORK SERVER READY port=%u evidence=listen-banner\n",port);return;}
            CHECK(now_ms()<deadline);pause_ms(10);
        }
    }
    for(;;){int fd=socket(AF_INET,SOCK_STREAM,0);CHECK(fd>=0);
        struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons((unsigned short)port),.sin_addr={htonl(INADDR_LOOPBACK)}};
        int result=connect(fd,(void *)&address,sizeof(address));int error=errno;CHECK(close(fd)==0);
        if(result==0){printf("NETWORK SERVER READY port=%u\n",port);break;}
        CHECK(error==ECONNREFUSED && now_ms()<deadline);pause_ms(10);
    }
}
static int controlled(const char *suite, const char *selection, const char *libc)
{
    int iperf=!strcmp(suite,"iperf");
    char *iperf_server[]={"./iperf3","-s","-p","5001","--forceflush",NULL};
    char *netperf_server[]={"./netserver","-D","-L","127.0.0.1","-p","12865",NULL};
    const char *iperf_names[]={"BASIC_UDP","BASIC_TCP","PARALLEL_UDP","PARALLEL_TCP","REVERSE_UDP","REVERSE_TCP"};
    const char *netperf_names[]={"UDP_STREAM","TCP_STREAM","UDP_RR","TCP_RR","TCP_CRR"};
    unsigned total=iperf?6:5,passed=0,executed=0;
    for(unsigned i=0;i<total;i++){
        if(!strcmp(selection,"udp5") && (!iperf || i!=2))continue;
        if(!strcmp(selection,"tcp") && i!=(iperf?1U:1U))continue;
        if(!strcmp(selection,"representative") && (iperf?(i!=1 && i!=3):i!=2))continue;
        char *const *server_args=iperf?iperf_server:netperf_server;
        printf("NETWORK SERVER BEGIN argv=");
        for(unsigned j=0;server_args[j];j++)printf("%s%s",j?" ":"",server_args[j]);
        puts("");
        pid_t server=spawn(server_args,"/tmp/network-server.out");ready_server(iperf?5001:12865,iperf);
        const char *name=iperf?iperf_names[i]:netperf_names[i];
        char window[48];snprintf(window,sizeof(window),"%s-%s",libc,name);
        char *args[32];unsigned n=0;
        if(iperf){
            char *base[]={"./iperf3","-c","127.0.0.1","-p","5001","-t","2","-i","0"};
            for(unsigned j=0;j<sizeof(base)/sizeof(base[0]);j++)args[n++]=base[j];
            if(!(i&1)){args[n++]="-u";args[n++]="-b";args[n++]="1000G";}
            if(i==2 || i==3){args[n++]="-P";args[n++]="5";}
            if(i>=4)args[n++]="-R";
        }else{
            char *base[]={"./netperf","-H","127.0.0.1","-p","12865","-t",(char *)name,"-l","1","--","-s","16k","-S","16k","-m","1k","-M","1k"};
            for(unsigned j=0;j<sizeof(base)/sizeof(base[0]);j++)args[n++]=base[j];
            if(i>=2){args[n++]="-r";args[n++]="64,64";args[n++]="-R";args[n++]="1";}
        }
        args[n]=NULL;printf("NETWORK COMMAND BEGIN name=%s argv=",name);
        for(unsigned j=0;j<n;j++)printf("%s%s",j?" ":"",args[j]);
        puts("");
        begin_window(window);
        passed+=(unsigned)wait_command(spawn(args,"/tmp/network-client.out"),name,15000,"/tmp/network-client.out");executed++;
        CHECK(kill(server,SIGTERM)==0 || errno==ESRCH);
        int status;CHECK(waitpid(server,&status,0)==server);
        printf("NETWORK SERVER END name=%s wait=%d intentional_signal=%d\n",name,status,SIGTERM);
        show_output("/tmp/network-server.out");
        cleanup();
        end_window(window);
        if(passed!=executed)break; /* A known first failure does not launch the remaining matrix. */
    }
    printf("NETWORK CONTROLLED suite=%s passed=%u executed=%u\n",suite,passed,executed);
    return passed==executed && executed!=0;
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);char libc[16],suite[16],selection[24],platform[16];
    FILE *config=fopen("/network-selection","r");CHECK(config!=NULL && fscanf(config,"%15s %15s %23s %15s",libc,suite,selection,platform)==4 && fclose(config)==0);
    setup_loopback(platform);
    cost_control=open("/proc/boaros_cost_control",O_WRONLY|O_CLOEXEC);CHECK(cost_control>=0||errno==ENOENT);
    const char *libcs[]={"musl","glibc"},*suites[]={"iperf","netperf"};
    int passed=1;
    for(unsigned l=0;l<2 && passed;l++)for(unsigned s=0;s<2 && passed;s++){
        if(strcmp(libc,"both") && strcmp(libc,libcs[l]))continue;
        if(strcmp(suite,"both") && strcmp(suite,suites[s]))continue;
        char directory[24];snprintf(directory,sizeof(directory),"/%s",libcs[l]);CHECK(chdir(directory)==0);
        char libraries[32];snprintf(libraries,sizeof(libraries),"/%s/lib",libcs[l]);CHECK(setenv("LD_LIBRARY_PATH",libraries,1)==0);
        printf("NETWORK INPUT libc=%s suite=%s selection=%s cwd=%s\n",libcs[l],suites[s],selection,directory);
        if(!strcmp(selection,"script")){
            char script[32];snprintf(script,sizeof(script),"./%s_testcode.sh",suites[s]);
            char *args[]={"./busybox","sh",script,NULL};puts("NETWORK COMMAND BEGIN name=original-script");
            passed=wait_command(spawn(args,NULL),"original-script",120000,NULL);
        }else passed=controlled(suites[s],selection,libcs[l]);
    }
    if(passed)drain_protocol();
    cleanup();printf("NETWORK CONSUMERS %s\n",passed?"PASS":"FAIL");
    if(!passed)return 1;
    reboot(RB_POWER_OFF);return 0;
}
