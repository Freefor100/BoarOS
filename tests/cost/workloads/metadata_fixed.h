/* Fixed namespace work, shared by unchanged-kernel controls and candidates. */
#include <sys/mman.h>
#include <sys/resource.h>
#include <stdint.h>
static uint64_t metadata_samples[4096];
static void metadata_pin(unsigned cpu)
{
    cpu_set_t mask;CPU_ZERO(&mask);CPU_SET(cpu,&mask);
    CHECK(sched_setaffinity(0,sizeof(mask),&mask)==0);
}
static unsigned metadata_cpus(void)
{
    cpu_set_t mask;CPU_ZERO(&mask);CHECK(sched_getaffinity(0,sizeof(mask),&mask)==0);
    CHECK(CPU_ISSET(0,&mask));return CPU_ISSET(1,&mask)?2:1;
}
static int metadata_order(const void *a,const void *b)
{
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y);
}
static void metadata_statistics(const char *name,unsigned count)
{
    qsort(metadata_samples,count,sizeof(*metadata_samples),metadata_order);
    struct rusage self,children;
    CHECK(!getrusage(RUSAGE_SELF,&self) && !getrusage(RUSAGE_CHILDREN,&children));
    printf("COST METADATA WORK %s %u\n",name,count);
    printf("COST METADATA LATENCY %s %llu %llu %llu\n",name,
        (unsigned long long)metadata_samples[count/2],
        (unsigned long long)metadata_samples[(count*95+99)/100-1],
        (unsigned long long)metadata_samples[count-1]);
    printf("COST METADATA RSS %s %ld %ld\n",name,self.ru_maxrss,children.ru_maxrss);
}
static void metadata_result(const char *name,unsigned count)
{ cost_end(name,0,0,0,0);metadata_statistics(name,count); }
static void metadata_directory_sync(const char *path)
{
    int fd=open(path,O_RDONLY|O_DIRECTORY);CHECK(fd>=0 && !fsync(fd) && !close(fd));
}
static void metadata_fixed_lookup(unsigned kind)
{
    static const char *names[]={"metadata-stat-held","metadata-fstat-held","metadata-open-held"};
    int held=open("/metadata-target",O_RDONLY);CHECK(held>=0);
    struct stat expected,actual;CHECK(!fstat(held,&expected));
    cost_begin();
    for(unsigned i=0;i<4096;i++) {
        uint64_t start=nanoseconds();
        if(!kind)CHECK(!stat("/metadata-target",&actual));
        else if(kind==1)CHECK(!fstat(held,&actual));
        else {int fd=open("/metadata-target",O_RDONLY);CHECK(fd>=0 && !fstat(fd,&actual) && !close(fd));}
        metadata_samples[i]=nanoseconds()-start;
    }
    metadata_result(names[kind],4096);
    CHECK(actual.st_ino==expected.st_ino && actual.st_size==8 && actual.st_mode==expected.st_mode);
    CHECK(!close(held));
}
static void metadata_create_loop(const char *directory,unsigned actor,unsigned count,uint64_t *samples)
{
    for(unsigned i=0;i<count;i++) {
        char name[96];snprintf(name,sizeof(name),"%s/%u-%u",directory,actor,i);
        uint64_t start=nanoseconds();
        int fd=open(name,O_CREAT|O_EXCL|O_RDWR,0640);CHECK(fd>=0);
        struct stat st;CHECK(!fstat(fd,&st) && !close(fd) && !unlink(name));
        samples[i]=nanoseconds()-start;
    }
}
static void metadata_fixed_create(void)
{
    cost_begin();metadata_create_loop("/metadata-a",0,2048,metadata_samples);
    metadata_directory_sync("/metadata-a");metadata_result("metadata-create",2048);
}
static void metadata_fixed_rename(int cross)
{
    const char *a="/metadata-a/rename-a",*b=cross?"/metadata-b/rename-b":"/metadata-a/rename-b";
    int fd=open(a,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0 && write(fd,"rename",6)==6 && !fsync(fd) && !close(fd));
    cost_begin();
    for(unsigned i=0;i<2048;i++) {
        uint64_t start=nanoseconds();CHECK(!rename(i&1?b:a,i&1?a:b));metadata_samples[i]=nanoseconds()-start;
    }
    metadata_directory_sync("/metadata-a");if(cross)metadata_directory_sync("/metadata-b");
    metadata_result(cross?"metadata-rename-cross":"metadata-rename-same",2048);
    char bytes[6];fd=open(a,O_RDONLY);CHECK(fd>=0 && read(fd,bytes,6)==6 && !memcmp(bytes,"rename",6) && !close(fd) && !unlink(a));
}
static void metadata_fixed_readdir(void)
{
    CHECK(!mkdir("/metadata-scan",0750));
    for(unsigned i=0;i<256;i++) {char path[64];snprintf(path,sizeof(path),"/metadata-scan/item-%03u",i);int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0 && !close(fd));}
    metadata_directory_sync("/metadata-scan");
    unsigned counts[64];
    cost_begin();
    for(unsigned i=0;i<64;i++) {
        uint64_t start=nanoseconds();DIR *directory=opendir("/metadata-scan");CHECK(directory);
        unsigned count=0;while(readdir(directory))count++;
        CHECK(!closedir(directory));metadata_samples[i]=nanoseconds()-start;counts[i]=count;
    }
    metadata_result("metadata-readdir",64);
    for(unsigned i=0;i<64;i++)CHECK(counts[i]==258);
    for(unsigned i=0;i<256;i++) {char path[64];snprintf(path,sizeof(path),"/metadata-scan/item-%03u",i);CHECK(!unlink(path));}
    CHECK(!rmdir("/metadata-scan"));
}
static void metadata_fixed_paired(int separate,unsigned cpus)
{
    uint64_t *samples=mmap(NULL,2048*sizeof(*samples),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    CHECK(samples!=MAP_FAILED);
    int ready[2],gate[2];CHECK(!pipe(ready) && !pipe(gate));
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        metadata_pin(cpus==2?1:0);char c='r';
        CHECK(write(ready[1],&c,1)==1 && read(gate[0],&c,1)==1);
        metadata_create_loop(separate?"/metadata-b":"/metadata-a",1,1024,samples+1024);
        _exit(0);
    }
    char c;CHECK(read(ready[0],&c,1)==1);metadata_pin(0);
    cost_begin();CHECK(write(gate[1],"g",1)==1);
    metadata_create_loop("/metadata-a",0,1024,samples);
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    metadata_directory_sync("/metadata-a");if(separate)metadata_directory_sync("/metadata-b");
    const char *name=separate?"metadata-paired-separate":"metadata-paired-shared";
    cost_end(name,0,0,0,0);
    memcpy(metadata_samples,samples,2048*sizeof(*samples));
    metadata_statistics(name,2048);
    CHECK(!munmap(samples,2048*sizeof(*samples)));
    CHECK(!close(ready[0]) && !close(ready[1]) && !close(gate[0]) && !close(gate[1]));
    metadata_pin(cpus==2?1:0);
}
enum metadata_parallel_kind { METADATA_PAIRED_STAT, METADATA_PAIRED_OPEN,
    METADATA_PAIRED_RENAME, METADATA_PAIRED_APPEND };
static void metadata_parallel_loop(enum metadata_parallel_kind kind,unsigned actor,unsigned count,
    int fd,uint64_t *samples,struct stat *observed)
{
    const char *directory=actor?"/metadata-b":"/metadata-a";
    char first[64],second[64],bytes[4096];
    snprintf(first,sizeof(first),"%s/paired-rename-a",directory);
    snprintf(second,sizeof(second),"%s/paired-rename-b",directory);
    memset(bytes,'A'+actor,sizeof(bytes));
    struct stat st={0};
    for(unsigned i=0;i<count;i++) {
        uint64_t start=nanoseconds();
        if(kind==METADATA_PAIRED_STAT)CHECK(!stat("/metadata-target",&st));
        else if(kind==METADATA_PAIRED_OPEN) {
            int opened=open("/metadata-target",O_RDONLY);
            CHECK(opened>=0 && !fstat(opened,&st) && !close(opened));
        } else if(kind==METADATA_PAIRED_RENAME)CHECK(!rename(i&1?second:first,i&1?first:second));
        else CHECK(write(fd,bytes,sizeof(bytes))==(ssize_t)sizeof(bytes));
        samples[i]=nanoseconds()-start;
    }
    if(kind<=METADATA_PAIRED_OPEN)*observed=st;
}
static void metadata_parallel_verify_append(int fd,unsigned actor,unsigned count)
{
    struct stat st;CHECK(!fstat(fd,&st) && st.st_size==(off_t)count*4096 && lseek(fd,0,SEEK_SET)==0);
    char bytes[4096];
    for(unsigned i=0;i<count;i++) {
        CHECK(read(fd,bytes,sizeof(bytes))==(ssize_t)sizeof(bytes));
        for(unsigned j=0;j<sizeof(bytes);j++)CHECK(bytes[j]=='A'+actor);
    }
}
static void metadata_parallel_paired(enum metadata_parallel_kind kind,unsigned cpus)
{
    static const char *names[]={"metadata-paired-stat","metadata-paired-open",
        "metadata-paired-rename","metadata-paired-append"};
    unsigned count=kind==METADATA_PAIRED_RENAME?1024:2048;
    size_t sample_bytes=2*count*sizeof(uint64_t);
    struct {struct stat observed[2];uint64_t samples[4096];} *shared;
    shared=mmap(NULL,sizeof(*shared),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    CHECK(shared!=MAP_FAILED);uint64_t *samples=shared->samples;
    int files[2]={-1,-1};
    if(kind>=METADATA_PAIRED_RENAME) {
        for(unsigned actor=0;actor<2;actor++) {
            char path[64];snprintf(path,sizeof(path),"/metadata-%c/paired-%s-a",actor?'b':'a',
                kind==METADATA_PAIRED_RENAME?"rename":"append");
            files[actor]=open(path,O_CREAT|O_EXCL|O_RDWR|O_APPEND,0600);CHECK(files[actor]>=0);
            CHECK(!fsync(files[actor]));
        }
        metadata_directory_sync("/metadata-a");metadata_directory_sync("/metadata-b");
    }
    int ready[2],gate[2];CHECK(!pipe(ready) && !pipe(gate));
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        metadata_pin(cpus==2?1:0);char c='r';
        CHECK(write(ready[1],&c,1)==1 && read(gate[0],&c,1)==1);
        metadata_parallel_loop(kind,1,count,files[1],samples+count,&shared->observed[1]);_exit(0);
    }
    char c;CHECK(read(ready[0],&c,1)==1);metadata_pin(0);
    cost_begin();CHECK(write(gate[1],"g",1)==1);
    metadata_parallel_loop(kind,0,count,files[0],samples,&shared->observed[0]);
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    if(kind==METADATA_PAIRED_APPEND)CHECK(!fsync(files[0]) && !fsync(files[1]));
    if(kind==METADATA_PAIRED_RENAME) {
        metadata_directory_sync("/metadata-a");metadata_directory_sync("/metadata-b");
    }
    cost_end(names[kind],0,0,0,0);memcpy(metadata_samples,samples,sample_bytes);
    metadata_statistics(names[kind],2*count);
    if(kind<=METADATA_PAIRED_OPEN) {
        struct stat expected;CHECK(!stat("/metadata-target",&expected));
        for(unsigned actor=0;actor<2;actor++)CHECK(shared->observed[actor].st_ino==expected.st_ino &&
            shared->observed[actor].st_size==8 && shared->observed[actor].st_mode==expected.st_mode);
    }
    if(kind>=METADATA_PAIRED_RENAME) {
        for(unsigned actor=0;actor<2;actor++) {
            char path[64];snprintf(path,sizeof(path),"/metadata-%c/paired-%s-a",actor?'b':'a',
                kind==METADATA_PAIRED_RENAME?"rename":"append");
            if(kind==METADATA_PAIRED_APPEND)metadata_parallel_verify_append(files[actor],actor,count);
            else {struct stat named,held;CHECK(!stat(path,&named) && !fstat(files[actor],&held) && named.st_ino==held.st_ino);}
            CHECK(!close(files[actor]) && !unlink(path));
        }
    }
    CHECK(!munmap(shared,sizeof(*shared)));
    CHECK(!close(ready[0]) && !close(ready[1]) && !close(gate[0]) && !close(gate[1]));
    metadata_pin(cpus==2?1:0);
}
static int metadata_fixed_main(void)
{
    unsigned cpus=metadata_cpus();metadata_pin(cpus==2?1:0);
    CHECK(!mkdir("/metadata-a",0750) && !mkdir("/metadata-b",0750));
    int fd=open("/metadata-target",O_CREAT|O_EXCL|O_RDWR,0644);
    CHECK(fd>=0 && write(fd,"metadata",8)==8 && !fsync(fd) && !close(fd));
    printf("COST METADATA CPUS %u\n",cpus);
    for(unsigned i=0;i<3;i++)metadata_fixed_lookup(i);
    metadata_fixed_create();metadata_fixed_rename(0);metadata_fixed_rename(1);metadata_fixed_readdir();
    metadata_fixed_paired(0,cpus);metadata_fixed_paired(1,cpus);
    if(access("/cost-metadata-parallel",F_OK)==0) {
        for(unsigned kind=METADATA_PAIRED_STAT;kind<=METADATA_PAIRED_APPEND;kind++)
            metadata_parallel_paired(kind,cpus);
    }
    cost_begin();uint64_t start=nanoseconds();metadata_directory_sync("/");metadata_samples[0]=nanoseconds()-start;
    metadata_result("metadata-final-sync",1);
    puts("COST PASS metadata");CHECK(!fflush(NULL) && !tcdrain(STDOUT_FILENO));return 0;
}
