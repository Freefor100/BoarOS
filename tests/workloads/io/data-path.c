#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <sys/resource.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#define MAX_FILES 4
#define MAX_REQUEST 65536
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "IO FAIL line=%d expression=%s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
static unsigned char buffer[MAX_REQUEST];
static unsigned files, request_bytes;
static uint64_t file_bytes;
static int cost_fd = -1;
static FILE *evidence;
static char result_path[1024];
static pid_t mixed_children[MAX_FILES];
static unsigned mixed_child_count;
static unsigned available_cpus[2],available_count;
struct io_interval { uint64_t start,end; };

static void choose_cpus(void)
{
    cpu_set_t mask;CPU_ZERO(&mask);
    CHECK(sched_getaffinity(0,sizeof(mask),&mask)==0);
    for (unsigned cpu=0;cpu<CPU_SETSIZE && available_count<2;cpu++)
        if (CPU_ISSET(cpu,&mask)) available_cpus[available_count++]=cpu;
    CHECK(available_count);
}
static unsigned pin_cpu(unsigned secondary)
{
    unsigned cpu=available_cpus[secondary && available_count>1];
    cpu_set_t mask,actual;CPU_ZERO(&mask);CPU_SET(cpu,&mask);
    CHECK(sched_setaffinity(0,sizeof(mask),&mask)==0 && sched_getaffinity(0,sizeof(actual),&actual)==0);
    CHECK(CPU_COUNT(&actual)==1 && CPU_ISSET(cpu,&actual));
    return cpu;
}
static void resources(void)
{
    struct rusage self,children;
    CHECK(!getrusage(RUSAGE_SELF,&self) && !getrusage(RUSAGE_CHILDREN,&children));
    fprintf(evidence,"IO RESOURCE self_maxrss_kib=%llu children_maxrss_kib=%llu\n",
        (unsigned long long)self.ru_maxrss,(unsigned long long)children.ru_maxrss);
}
struct result {
    int fd;
    uint64_t bytes, calls, start, end, sync_end, size_before, size_after, checksum;
    uint64_t growth[5];
    unsigned growth_count;
};

static uint64_t now(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000U + (uint64_t)t.tv_nsec;
}

static unsigned char pattern(unsigned id, uint64_t offset, unsigned epoch)
{
    uint32_t word = ((uint32_t)id * 0x9e3779b9U) ^ ((uint32_t)(offset / 4) * 0x85ebca6bU) ^ (epoch * 0xc2b2ae35U);
    return (unsigned char)(word >> ((offset % 4) * 8));
}

static void fill(unsigned id, uint64_t offset, size_t count, unsigned epoch)
{
    for (size_t j = 0; j < count; ++j) buffer[j] = pattern(id, offset + j, epoch);
}

static uint64_t check(unsigned id, uint64_t offset, size_t count, unsigned epoch)
{
    uint64_t checksum = 0;
    for (size_t j = 0; j < count; ++j) {
        CHECK(buffer[j] == pattern(id, offset + j, epoch));
        checksum += buffer[j];
    }
    return checksum;
}

static void exact(int fd, size_t count, int writing, uint64_t *calls)
{
    size_t done = 0;
    while (done < count) {
        ssize_t n = writing ? write(fd, buffer + done, count - done) : read(fd, buffer + done, count - done);
        if (calls) ++*calls;
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0 && (size_t)n <= count - done);
        done += (size_t)n;
    }
}

static uint64_t size_of(int fd)
{
    struct stat s;
    CHECK(fstat(fd, &s) == 0 && S_ISREG(s.st_mode) && s.st_size >= 0);
    return (uint64_t)s.st_size;
}

static uint64_t verify(int fd, unsigned id, unsigned epoch)
{
    CHECK(size_of(fd) == file_bytes && lseek(fd, 0, SEEK_SET) == 0);
    uint64_t checksum = 0;
    for (uint64_t offset = 0; offset < file_bytes;) {
        size_t count = file_bytes - offset;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        exact(fd, count, 0, NULL);
        checksum += check(id, offset, count, epoch);
        offset += count;
    }
    CHECK(read(fd, buffer, 1) == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    return checksum;
}

static void report_proc(const char *path, const char *name)
{
    FILE *input = fopen(path, "r"); CHECK(input);
    fprintf(evidence, "IO %s BEGIN\n", name);
    char chunk[4096]; size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), input)) != 0)
        CHECK(fwrite(chunk, 1, n, evidence) == n);
    CHECK(!ferror(input) && fclose(input) == 0);
    fprintf(evidence, "IO %s END\n", name);
}

static void stop_observation(void)
{
    if (cost_fd < 0) { evidence = fopen(result_path, "w"); CHECK(evidence); return; }
    uint64_t deadline = now() + 10000000000ULL;
    while (write(cost_fd, "end\n", 4) != 4) {
        CHECK(errno == EBUSY && now() < deadline);
        struct timespec pause = {.tv_nsec = 20000000};
        (void)nanosleep(&pause, NULL);
    }
    evidence = fopen(result_path, "w"); CHECK(evidence);
    report_proc("/proc/boaros_mem_stats", "MEM_PEAK");
    report_proc("/proc/boaros_cost", "COST");
    report_proc("/proc/meminfo", "MEM_AFTER");
    CHECK(close(cost_fd) == 0);
}

static void start_observation(void)
{
    if (access("/observe", F_OK) != 0) return;
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) CHECK(errno == EBUSY);
    cost_fd = open("/proc/boaros_cost_control", O_WRONLY | O_CLOEXEC); CHECK(cost_fd >= 0);
    CHECK(write(cost_fd, "begin\n", 6) == 6);
}

struct mixed_sample { uint64_t open_start, start, end; };
struct mixed_report {
    unsigned id, sample_count, cpu;
    uint64_t start, end, data_start, data_end, sync_start, sync_end;
    uint64_t bytes, calls, checksum, growth[5];
    unsigned growth_count;
};

static void transfer(int fd, void *storage, size_t length, int writing)
{
    unsigned char *cursor = storage;
    while (length) {
        ssize_t n = writing ? write(fd, cursor, length) : read(fd, cursor, length);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0 && (size_t)n <= length);
        cursor += n;
        length -= (size_t)n;
    }
}

static int mixed_open(const char *directory, unsigned id, int flags)
{
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/file-%u", directory, id);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    int fd = open(path, flags); CHECK(fd >= 0);
    return fd;
}

static void mixed_child(const char *directory, unsigned id, int ready, int gate, int result, int appending)
{
    mixed_child_count = 0;
    unsigned count = appending ? (unsigned)((file_bytes + request_bytes - 1) / request_bytes) :
        id == 0 ? 0 : id == 3 ? 256 : (unsigned)((file_bytes + request_bytes - 1) / request_bytes);
    struct mixed_sample *samples = count ? calloc(count, sizeof(*samples)) : NULL;
    CHECK(!count || samples);
    /* 先触及记录页，样本写入自身的首次缺页不混进读取窗口。 */
    for (size_t offset = 0; offset < count * sizeof(*samples); offset += 4096)
        ((volatile unsigned char *)samples)[offset] = 0;
    struct mixed_report report = {.id = id, .sample_count = count, .cpu=pin_cpu(id==1)};
    int fd = mixed_open(directory, id, appending || !id ? O_RDWR | O_APPEND : O_RDONLY);
    CHECK(size_of(fd) == (appending || !id ? 0 : file_bytes));
    if (!appending && !id) {
        report.data_start = now();
        for (uint64_t offset = 0; offset < file_bytes;) {
            size_t length = file_bytes - offset;
            if (length > request_bytes) length = request_bytes;
            fill(id, offset, length, 1);
            exact(fd, length, 1, &report.calls);
            offset += length;
        }
        report.data_end = now();
        CHECK(size_of(fd) == file_bytes);
        report.bytes = file_bytes;
    }
    transfer(ready, &id, sizeof(id), 1);
    unsigned char go;
    transfer(gate, &go, sizeof(go), 0);
    CHECK(go == 1 && close(gate) == 0 && close(ready) == 0);
    report.start = now();
    if (appending) {
        report.data_start = report.start;
        for (unsigned sequence=0; sequence<count; sequence++) {
            uint64_t offset=(uint64_t)sequence*request_bytes;
            size_t length=file_bytes-offset;
            if (length>request_bytes) length=request_bytes;
            fill(id,offset,length,1);
            samples[sequence].start=now();
            exact(fd,length,1,&report.calls);
            samples[sequence].end=now();
            report.bytes+=length;
            if (!offset || report.bytes==file_bytes/4 || report.bytes==file_bytes/2 ||
                report.bytes==file_bytes*3/4 || report.bytes==file_bytes) {
                CHECK(report.growth_count<5 && size_of(fd)==report.bytes);
                report.growth[report.growth_count++]=report.bytes;
            }
        }
        report.data_end=now();
        report.sync_start=report.data_end;
        CHECK(!fsync(fd)); report.sync_end=now();
    } else if (!id) {
        report.sync_start = now();
        CHECK(fsync(fd) == 0);
        report.sync_end = now();
    } else if (id < 3) {
        for (unsigned sequence = 0; sequence < count; ++sequence) {
            uint64_t offset = (uint64_t)sequence * request_bytes;
            size_t length = file_bytes - offset;
            if (length > request_bytes) length = request_bytes;
            samples[sequence].start = now();
            exact(fd, length, 0, &report.calls);
            samples[sequence].end = now();
            report.checksum += check(id, offset, length, 0);
            report.bytes += length;
        }
        CHECK(read(fd, buffer, 1) == 0);
    } else {
        /* 第一项已经持有最后一个 FD；后续项同时保留 open 与 close 的等待时间。 */
        for (unsigned sequence = 0; sequence < count; ++sequence) {
            if (sequence) {
                samples[sequence].open_start = now();
                fd = mixed_open(directory, id, O_RDONLY);
            }
            samples[sequence].start = now();
            CHECK(close(fd) == 0);
            samples[sequence].end = now();
            ++report.calls;
        }
        fd = -1;
    }
    report.end = now();
    if (fd >= 0) CHECK(close(fd) == 0);
    transfer(result, &report, sizeof(report), 1);
    if (count) transfer(result, samples, count * sizeof(*samples), 1);
    CHECK(close(result) == 0);
    free(samples);
    _exit(0);
}

static int mixed_read(const char *directory, int appending)
{
    int ready[2], gates[MAX_FILES][2], reports[MAX_FILES][2];
    CHECK(pipe(ready) == 0);
    for (unsigned id = 0; id < files; ++id)
        CHECK(pipe(gates[id]) == 0 && pipe(reports[id]) == 0);
    for (unsigned id = 0; id < files; ++id) {
        pid_t child = fork(); CHECK(child >= 0);
        if (!child) {
            CHECK(close(ready[0]) == 0);
            for (unsigned other = 0; other < files; ++other) {
                CHECK(close(gates[other][1]) == 0 && close(reports[other][0]) == 0);
                if (other != id) CHECK(close(gates[other][0]) == 0 && close(reports[other][1]) == 0);
            }
            mixed_child(directory, id, ready[1], gates[id][0], reports[id][1], appending);
        }
        mixed_children[mixed_child_count++] = child;
    }
    CHECK(close(ready[1]) == 0);
    unsigned seen = 0;
    for (unsigned i = 0; i < files; ++i) {
        unsigned id;
        transfer(ready[0], &id, sizeof(id), 0);
        CHECK(id < files && !(seen & (1U << id)));
        seen |= 1U << id;
    }
    CHECK(close(ready[0]) == 0);
    start_observation();
    uint64_t start = now();
    for (unsigned id = 0; id < files; ++id) {
        CHECK(close(gates[id][0]) == 0 && close(reports[id][1]) == 0);
        unsigned char go = 1;
        transfer(gates[id][1], &go, sizeof(go), 1);
        CHECK(close(gates[id][1]) == 0);
    }
    struct mixed_report completed[MAX_FILES];
    struct mixed_sample *samples[MAX_FILES] = {0};
    for (unsigned id = 0; id < files; ++id) {
        transfer(reports[id][0], &completed[id], sizeof(completed[id]), 0);
        CHECK(completed[id].id == id);
        unsigned count = completed[id].sample_count;
        CHECK(count == (appending ? (file_bytes + request_bytes - 1) / request_bytes :
            id == 0 ? 0 : id == 3 ? 256 : (file_bytes + request_bytes - 1) / request_bytes));
        if (count) {
            samples[id] = calloc(count, sizeof(*samples[id])); CHECK(samples[id]);
            transfer(reports[id][0], samples[id], count * sizeof(*samples[id]), 0);
        }
        CHECK(close(reports[id][0]) == 0);
    }
    for (unsigned id = 0; id < mixed_child_count; ++id) {
        int status;
        CHECK(waitpid(mixed_children[id], &status, 0) == mixed_children[id]);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    mixed_child_count = 0;
    uint64_t end = now();
    stop_observation();
    uint64_t verification_started=now();
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/file-0", directory);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    int fd = open(path, O_RDONLY); CHECK(fd >= 0);
    completed[0].checksum = verify(fd, 0, 1);
    CHECK(close(fd) == 0);
    if (appending) {
        uint64_t data_end=0,sync_end=0;
        for (unsigned id=0;id<files;id++) {
            if (completed[id].data_end>data_end) data_end=completed[id].data_end;
            if (completed[id].sync_end>sync_end) sync_end=completed[id].sync_end;
            if (id) {
                int other=mixed_open(directory,id,O_RDONLY);
                completed[id].checksum=verify(other,id,1); CHECK(!close(other));
            }
        }
        CHECK(start<data_end && data_end<=sync_end && sync_end<=end);
        fprintf(evidence,"IO WINDOW start_ns=%llu data_end_ns=%llu end_ns=%llu elapsed_ns=%llu data_ns=%llu sync_ns=%llu bytes=%llu sync_calls=%u verify_ns=%llu cleanup_sync_ns=0 cleanup_sync_calls=0 complete=1\n",
            (unsigned long long)start,(unsigned long long)data_end,(unsigned long long)sync_end,
            (unsigned long long)(sync_end-start),(unsigned long long)(data_end-start),
            (unsigned long long)(sync_end-data_end),(unsigned long long)(file_bytes*files),files,
            (unsigned long long)(now()-verification_started));
    } else fprintf(evidence, "IO MIXED_WINDOW start_ns=%llu end_ns=%llu bytes=%llu complete=1\n",
            (unsigned long long)start, (unsigned long long)end, (unsigned long long)(file_bytes * 2));
    for (unsigned id = 0; id < files; ++id) {
        const struct mixed_report *r = &completed[id];
        fprintf(evidence,"IO AFFINITY role=%u cpu=%u\n",id,r->cpu);
        if (appending) {
            fprintf(evidence,"IO FILE id=%u bytes=%llu calls=%llu start_ns=%llu end_ns=%llu sync_end_ns=%llu size_before=0 size_after=%llu checksum=%llu complete=1\n",
                id,(unsigned long long)r->bytes,(unsigned long long)r->calls,(unsigned long long)r->start,
                (unsigned long long)r->data_end,(unsigned long long)r->sync_end,
                (unsigned long long)file_bytes,(unsigned long long)r->checksum);
            CHECK(r->growth_count==5);
            for (unsigned j=0;j<r->growth_count;j++)
                fprintf(evidence,"IO SIZE id=%u sample=%u bytes=%llu\n",id,j,(unsigned long long)r->growth[j]);
        } else fprintf(evidence, "IO MIXED_FILE id=%u bytes=%llu calls=%llu start_ns=%llu end_ns=%llu data_start_ns=%llu data_end_ns=%llu sync_start_ns=%llu sync_end_ns=%llu checksum=%llu samples=%u complete=1\n",
                id, (unsigned long long)r->bytes, (unsigned long long)r->calls,
                (unsigned long long)r->start, (unsigned long long)r->end,
                (unsigned long long)r->data_start, (unsigned long long)r->data_end,
                (unsigned long long)r->sync_start, (unsigned long long)r->sync_end,
                (unsigned long long)r->checksum, r->sample_count);
        for (unsigned sequence = 0; sequence < r->sample_count; ++sequence) {
            if (appending)
                fprintf(evidence,"IO SAMPLE id=%u sequence=%u start_ns=%llu end_ns=%llu\n",id,sequence,
                    (unsigned long long)samples[id][sequence].start,(unsigned long long)samples[id][sequence].end);
            else fprintf(evidence, "IO MIXED_SAMPLE id=%u sequence=%u open_start_ns=%llu start_ns=%llu end_ns=%llu\n",
                    id, sequence, (unsigned long long)samples[id][sequence].open_start,
                    (unsigned long long)samples[id][sequence].start, (unsigned long long)samples[id][sequence].end);
        }
        free(samples[id]);
    }
    resources();
    fputs("IO PASS all\n", evidence);
    CHECK(fflush(evidence) == 0 && fsync(fileno(evidence)) == 0 && fclose(evidence) == 0);
    puts("IO PASS all");
    return 0;
}

static int range_modify(const char *directory, unsigned cpu)
{
    int fd=mixed_open(directory,0,O_RDWR);
    CHECK(size_of(fd)==file_bytes && file_bytes>=2*request_bytes);
    size_t count=file_bytes/request_bytes;
    struct io_interval *samples=calloc(count,sizeof(*samples));CHECK(samples);
    for (size_t i=0;i<count*sizeof(*samples);i+=4096) ((volatile unsigned char *)samples)[i]=0;
    uint64_t calls=0;
    start_observation();uint64_t start=now();
    for (size_t i=0;i<count;i++) {
        samples[i].start=now();
        CHECK(!ftruncate(fd,file_bytes-request_bytes));calls++;
        CHECK(!fallocate(fd,FALLOC_FL_KEEP_SIZE,file_bytes,request_bytes));calls++;
        CHECK(!fallocate(fd,0,file_bytes-2*request_bytes,2*request_bytes));calls++;
        CHECK(!fallocate(fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,
            file_bytes-2*request_bytes,request_bytes));calls++;
        samples[i].end=now();
    }
    uint64_t data_end=now();CHECK(!fsync(fd));uint64_t end=now();stop_observation();
    uint64_t verify_start=now(),checksum=0;
    CHECK(size_of(fd)==file_bytes && lseek(fd,0,SEEK_CUR)==0);
    for (uint64_t offset=0;offset<file_bytes;) {
        size_t length=file_bytes-offset;if(length>sizeof(buffer))length=sizeof(buffer);
        exact(fd,length,0,NULL);
        for (size_t j=0;j<length;j++) {
            unsigned char expected=offset+j<file_bytes-2*request_bytes?pattern(0,offset+j,0):0;
            CHECK(buffer[j]==expected);checksum+=buffer[j];
        }
        offset+=length;
    }
    CHECK(read(fd,buffer,1)==0);uint64_t verify_end=now();
    fprintf(evidence,"IO WINDOW start_ns=%llu data_end_ns=%llu end_ns=%llu elapsed_ns=%llu data_ns=%llu sync_ns=%llu bytes=0 sync_calls=1 verify_ns=%llu cleanup_sync_ns=0 cleanup_sync_calls=0 range_operations=%llu complete=1\n",
        (unsigned long long)start,(unsigned long long)data_end,(unsigned long long)end,
        (unsigned long long)(end-start),(unsigned long long)(data_end-start),(unsigned long long)(end-data_end),
        (unsigned long long)(verify_end-verify_start),(unsigned long long)calls);
    fprintf(evidence,"IO AFFINITY role=0 cpu=%u\n",cpu);
    fprintf(evidence,"IO FILE id=0 bytes=0 calls=%llu start_ns=%llu end_ns=%llu sync_end_ns=%llu size_before=%llu size_after=%llu checksum=%llu complete=1\n",
        (unsigned long long)calls,(unsigned long long)start,(unsigned long long)data_end,
        (unsigned long long)end,(unsigned long long)file_bytes,(unsigned long long)file_bytes,(unsigned long long)checksum);
    for(size_t i=0;i<count;i++)fprintf(evidence,"IO SAMPLE id=0 sequence=%zu start_ns=%llu end_ns=%llu\n",
        i,(unsigned long long)samples[i].start,(unsigned long long)samples[i].end);
    CHECK(!close(fd));free(samples);resources();fputs("IO PASS all\n",evidence);
    CHECK(!fflush(evidence) && !fsync(fileno(evidence)) && !fclose(evidence));puts("IO PASS all");return 0;
}

/* 固定写策略窗口共用结果格式；旧data-path模式保留原协议。 */
#define POLICY_SAMPLES 40000
#define MIB (1024ULL * 1024ULL)
struct policy_sample { uint64_t start,end,offset,bytes; unsigned kind; };
struct policy_actor {
    unsigned id,cpu,count; uint64_t bytes,calls,checksum,start,end,user_ns,system_ns;
};
static struct policy_sample *policy_samples;
static struct policy_actor policy_actor;
static uint64_t policy_user_start,policy_system_start;

static void policy_record(unsigned kind,uint64_t begin,uint64_t offset,uint64_t bytes)
{
    CHECK(policy_actor.count<POLICY_SAMPLES);
    policy_samples[policy_actor.count++]=(struct policy_sample){begin,now(),offset,bytes,kind};
}
static void policy_begin(unsigned id)
{
    memset(&policy_actor,0,sizeof(policy_actor));policy_actor.id=id;policy_actor.cpu=pin_cpu(id==1);
    policy_samples=calloc(POLICY_SAMPLES,sizeof(*policy_samples));CHECK(policy_samples);
    for(size_t i=0;i<POLICY_SAMPLES*sizeof(*policy_samples);i+=4096)((volatile char *)policy_samples)[i]=0;
}
static void policy_usage_reset(void)
{
    struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));
    policy_user_start=(uint64_t)usage.ru_utime.tv_sec*1000000000ULL+usage.ru_utime.tv_usec*1000ULL;
    policy_system_start=(uint64_t)usage.ru_stime.tv_sec*1000000000ULL+usage.ru_stime.tv_usec*1000ULL;
}
static void policy_usage(void)
{
    struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));
    policy_actor.user_ns=(uint64_t)usage.ru_utime.tv_sec*1000000000ULL+usage.ru_utime.tv_usec*1000ULL-policy_user_start;
    policy_actor.system_ns=(uint64_t)usage.ru_stime.tv_sec*1000000000ULL+usage.ru_stime.tv_usec*1000ULL-policy_system_start;
}
static void policy_print(const struct policy_actor *r,const struct policy_sample *samples)
{
    fprintf(evidence,"IO AFFINITY role=%u cpu=%u\n",r->id,r->cpu);
    fprintf(evidence,"IO POLICY_FILE id=%u bytes=%llu calls=%llu checksum=%llu start_ns=%llu end_ns=%llu samples=%u user_ns=%llu system_ns=%llu complete=1\n",
        r->id,(unsigned long long)r->bytes,(unsigned long long)r->calls,(unsigned long long)r->checksum,
        (unsigned long long)r->start,(unsigned long long)r->end,r->count,(unsigned long long)r->user_ns,(unsigned long long)r->system_ns);
    for(unsigned i=0;i<r->count;i++)fprintf(evidence,"IO POLICY_SAMPLE id=%u sequence=%u kind=%u start_ns=%llu end_ns=%llu offset=%llu bytes=%llu\n",
        r->id,i,samples[i].kind,(unsigned long long)samples[i].start,(unsigned long long)samples[i].end,
        (unsigned long long)samples[i].offset,(unsigned long long)samples[i].bytes);
}
static void policy_window(uint64_t start,uint64_t data_end,uint64_t end,uint64_t bytes,uint64_t distinct,uint64_t prep,unsigned sync_calls)
{
    fprintf(evidence,"IO POLICY_WINDOW start_ns=%llu data_end_ns=%llu end_ns=%llu elapsed_ns=%llu bytes=%llu distinct_bytes=%llu prepare_ns=%llu sync_calls=%u complete=1\n",
        (unsigned long long)start,(unsigned long long)data_end,(unsigned long long)end,(unsigned long long)(end-start),
        (unsigned long long)bytes,(unsigned long long)distinct,(unsigned long long)prep,sync_calls);
}
static int policy_finish(void)
{
    resources();fputs("IO PASS all\n",evidence);
    CHECK(!fflush(evidence) && !fsync(fileno(evidence)) && !fclose(evidence));puts("IO PASS all");return 0;
}
static void wait_until(uint64_t deadline)
{
    uint64_t current;
    while((current=now())<deadline){uint64_t delay=deadline-current;struct timespec pause={delay/1000000000ULL,delay%1000000000ULL};
        CHECK(!nanosleep(&pause,NULL) || errno==EINTR);}
}
static void policy_sync(int fd,int data_only)
{
    uint64_t begin=now();CHECK(!(data_only?fdatasync(fd):fsync(fd)));policy_record(2,begin,0,0);
}
static void policy_pwrite(int fd,unsigned id,uint64_t offset,size_t length,unsigned epoch)
{
    fill(id,offset,length,epoch);uint64_t begin=now();
    CHECK(pwrite(fd,buffer,length,(off_t)offset)==(ssize_t)length);
    policy_record(1,begin,offset,length);policy_actor.bytes+=length;policy_actor.calls++;
}
static uint64_t policy_verify(int fd,unsigned id,unsigned epoch,uint64_t length)
{
    uint64_t saved=file_bytes;file_bytes=length;uint64_t sum=verify(fd,id,epoch);file_bytes=saved;return sum;
}
static int policy_single(const char *directory,const char *operation,const char *completion)
{
    unsigned segments=0;char temperature[8],layout[8];
    int vector=sscanf(operation,"writev-%u-%7[^-]-%7s",&segments,temperature,layout)==3;
    int sync_write=!strcmp(operation,"sync-write"),hot=!strcmp(operation,"hot-write"),sparse=!strcmp(operation,"sparse-write");
    int age_hot=!strcmp(operation,"age-hot"),age_burst=!strcmp(operation,"age-burst"),stream=!strcmp(operation,"stream-write");
    CHECK(vector || sync_write || hot || sparse || age_hot || age_burst || stream);
    CHECK(files==1);
    int sync_open=!strcmp(completion,"osync")?O_SYNC:!strcmp(completion,"odsync")?O_DSYNC:0;
    int periodic=!strcmp(completion,"fsync16") || !strcmp(completion,"fdatasync16");
    CHECK(sync_write ? ((sync_open || periodic) && file_bytes==4*MIB && request_bytes==4096) : !strcmp(completion,"fsync"));
    if(vector)CHECK((segments==1 || segments==4 || segments==64) && (!strcmp(temperature,"hot") || !strcmp(temperature,"cold")) &&
        (!strcmp(layout,"same") || !strcmp(layout,"split")) && file_bytes==16*MIB && request_bytes==4096);
    if(hot || age_hot)CHECK(file_bytes==16*MIB && request_bytes==4096);
    if(age_burst || sparse)CHECK(file_bytes==4*MIB);
    if(stream)CHECK(file_bytes==1024*MIB && request_bytes==65536);
    policy_begin(0);
    uint64_t prepare=now(),length=hot || age_hot ? MIB:file_bytes;
    int fd=mixed_open(directory,0,O_RDWR|sync_open);CHECK(size_of(fd)==(stream?0:length));
    if(vector && !strcmp(temperature,"hot")) (void)policy_verify(fd,0,0,length);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);CHECK(page>=4096 && page<=65536);
    unsigned char *source=NULL;struct iovec vectors[64];
    if(vector){CHECK(!posix_memalign((void **)&source,page,page*64));memset(source,0,page*64);}
    prepare=now()-prepare;
    start_observation();policy_usage_reset();uint64_t start=now();policy_actor.start=start;
    unsigned sync_calls=0;
    if(sparse){
        CHECK((request_bytes==1024 || request_bytes==4096) && request_bytes<=page);
        unsigned blocks=page==request_bytes?1:2;
        for(unsigned epoch=1;epoch<=16;epoch++)for(uint64_t offset=0;offset<length;offset+=page)
            for(unsigned edge=0;edge<blocks;edge++)policy_pwrite(fd,0,offset+(edge?page-request_bytes:0),request_bytes,epoch);
    }else{
        for(uint64_t logical=0;logical<file_bytes;logical+=request_bytes){
            uint64_t offset=(hot || age_hot)?logical%MIB:logical;
            unsigned epoch=(hot || age_hot)?1+logical/MIB:1;
            if(age_hot && !(logical%MIB))wait_until(start+(logical/MIB)*6000000000ULL/16);
            if(vector){
                size_t part=4096/segments;
                for(unsigned i=0;i<segments;i++){
                    unsigned char *target=source+(!strcmp(layout,"split")?i*page:i*part);
                    for(size_t j=0;j<part;j++)target[j]=pattern(0,offset+i*part+j,1);
                    vectors[i]=(struct iovec){target,part};
                }
                uint64_t begin=now();CHECK(writev(fd,vectors,segments)==4096);policy_record(1,begin,offset,4096);
                policy_actor.bytes+=4096;policy_actor.calls++;
            }else policy_pwrite(fd,0,offset,request_bytes,epoch);
            if(periodic && (logical/request_bytes+1)%16==0){policy_sync(fd,!strcmp(completion,"fdatasync16"));sync_calls++;}
        }
    }
    uint64_t data_end=now();
    if(age_hot || age_burst)wait_until(start+6000000000ULL);
    if(!sync_open && !periodic){policy_sync(fd,0);sync_calls++;}
    uint64_t end=now();policy_actor.end=end;policy_usage();stop_observation();
    uint64_t distinct=length;
    if(sparse){
        struct statfs fs;CHECK(!fstatfs(fd,&fs));
        fprintf(evidence,"IO POLICY_LAYOUT page_bytes=%zu block_bytes=%u filesystem_block_bytes=%llu\n",page,request_bytes,(unsigned long long)fs.f_bsize);
        CHECK(!lseek(fd,0,SEEK_SET));uint64_t sum=0;
        for(uint64_t offset=0;offset<length;offset+=page){exact(fd,page,0,NULL);for(size_t j=0;j<page;j++){
            int changed=j<request_bytes || j>=page-request_bytes;
            CHECK(buffer[j]==pattern(0,offset+j,changed?16:0));sum+=buffer[j];}}
        CHECK(read(fd,buffer,1)==0);policy_actor.checksum=sum;distinct=length/page*(page==request_bytes?1:2)*request_bytes;
    }else policy_actor.checksum=policy_verify(fd,0,(hot || age_hot)?16:1,length);
    CHECK(!close(fd));free(source);
    policy_window(start,data_end,end,policy_actor.bytes,distinct,prepare,sync_calls);policy_print(&policy_actor,policy_samples);free(policy_samples);
    return policy_finish();
}

struct policy_event { unsigned id,round; uint64_t when; };
struct policy_round { unsigned id,round; uint64_t ready,go,done; };
static void policy_event_send(int fd,unsigned id,unsigned round)
{ struct policy_event e={id,round,now()};transfer(fd,&e,sizeof(e),1); }
static void policy_go(int fd)
{ unsigned char go;transfer(fd,&go,1,0);CHECK(go==1); }
static void policy_release(int fd)
{ unsigned char go=1;transfer(fd,&go,1,1); }

static void policy_child(const char *directory,const char *second,const char *operation,unsigned id,int ready,int gate,int result)
{
    mixed_child_count=0;policy_begin(id);
    int periodic=!strcmp(operation,"periodic-sync"),active=!strcmp(operation,"active-mixed");
    int mapped=!strcmp(operation,"mmap-write"),paired=mapped || !strcmp(operation,"pwrite-pair");
    int domain=!strncmp(operation,"domain-",7),busy=!strcmp(operation,"domain-busy");
    int fd=mixed_open(domain && id?second:directory,paired?0:id,active && id?O_RDONLY:O_RDWR);
    uint64_t length=paired?4*MIB:active?(id?4*MIB:0):domain?0:MIB;CHECK(size_of(fd)==length);
    size_t page=sysconf(_SC_PAGESIZE);unsigned char *mapping=NULL;
    if(mapped && id==1){mapping=mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(mapping!=MAP_FAILED);}
    if(active && id==3){CHECK(!close(fd));fd=-1;}
    policy_event_send(ready,id,0);policy_go(gate);policy_usage_reset();policy_actor.start=now();
    if(periodic){
        for(unsigned round=0;round<16;round++){
            for(unsigned i=0;i<16;i++)policy_pwrite(fd,id,(uint64_t)round*65536+i*4096,4096,1);
            policy_event_send(ready,id,round);policy_go(gate);policy_sync(fd,0);policy_event_send(ready,id,round);
            /* DONE齐全后才允许下一轮写入。 */
            policy_go(gate);
        }
    }else if(active){
        if(!id){
            for(uint64_t offset=0;offset<16*MIB;offset+=4096){
                policy_pwrite(fd,0,offset,4096,1);
                if((offset+4096)%MIB==0){
                    if(offset+4096==MIB){policy_event_send(ready,id,0);policy_go(gate);}
                    policy_sync(fd,1);
                }
            }
            policy_sync(fd,0);
        }else if(id<3){
            for(uint64_t offset=0;offset<4*MIB;offset+=4096){uint64_t begin=now();exact(fd,4096,0,&policy_actor.calls);
                policy_record(3,begin,offset,4096);policy_actor.bytes+=4096;policy_actor.checksum+=check(id,offset,4096,0);}
            CHECK(read(fd,buffer,1)==0);
        }else{
            for(unsigned i=0;i<256;i++){
                uint64_t begin=now();fd=mixed_open(directory,id,O_RDONLY);policy_record(5,begin,0,0);policy_actor.calls++;
                begin=now();CHECK(!close(fd));policy_record(4,begin,0,0);policy_actor.calls++;
            }fd=-1;
        }
    }else if(paired){
        uint64_t slots=4*MIB/page;
        for(uint64_t i=0;i<8*MIB/1024;i++){
            uint64_t offset=(i%slots)*page+id*1024;unsigned epoch=1+i/slots;
            if(mapping){fill(0,offset,1024,epoch);uint64_t begin=now();memcpy(mapping+offset,buffer,1024);
                policy_record(1,begin,offset,1024);policy_actor.bytes+=1024;policy_actor.calls++;
            }else policy_pwrite(fd,0,offset,1024,epoch);
        }
        policy_event_send(ready,id,0);policy_go(gate);
        if(!id)policy_sync(fd,0);
    }else{
        CHECK(domain);
        uint64_t amount=busy?512*MIB:id?0:1024*MIB;
        for(uint64_t offset=0;offset<amount;offset+=65536)policy_pwrite(fd,id,offset,65536,1);
        policy_event_send(ready,id,0);policy_go(gate);policy_sync(fd,0);
    }
    policy_actor.end=now();policy_usage();
    if(mapping)CHECK(!munmap(mapping,length));
    if(fd>=0)CHECK(!close(fd));
    transfer(result,&policy_actor,sizeof(policy_actor),1);transfer(result,policy_samples,policy_actor.count*sizeof(*policy_samples),1);
    CHECK(!close(result));free(policy_samples);_exit(0);
}

static int policy_parallel(const char *directory,const char *second,const char *operation,const char *completion)
{
    int periodic=!strcmp(operation,"periodic-sync"),active=!strcmp(operation,"active-mixed");
    int paired=!strcmp(operation,"mmap-write") || !strcmp(operation,"pwrite-pair"),domain=!strncmp(operation,"domain-",7);
    CHECK(!strcmp(completion,"fsync"));
    CHECK((periodic && files==4 && file_bytes==MIB && request_bytes==4096) ||
        (active && files==4 && file_bytes==16*MIB && request_bytes==4096) ||
        (paired && files==2 && file_bytes==4*MIB && request_bytes==1024) ||
        (domain && second && files==2 && request_bytes==65536 && file_bytes==(!strcmp(operation,"domain-busy")?512*MIB:1024*MIB)));
    struct stat domains[2];
    if(domain){CHECK(!stat(directory,&domains[0]) && !stat(second,&domains[1]) && domains[0].st_dev!=domains[1].st_dev);}
    uint64_t prepare=now();int ready[2],gates[MAX_FILES][2],reports[MAX_FILES][2];CHECK(!pipe(ready));
    for(unsigned id=0;id<files;id++)CHECK(!pipe(gates[id]) && !pipe(reports[id]));
    for(unsigned id=0;id<files;id++){
        pid_t child=fork();CHECK(child>=0);
        if(!child){CHECK(!close(ready[0]));for(unsigned other=0;other<files;other++){
            CHECK(!close(gates[other][1]) && !close(reports[other][0]));
            if(other!=id)CHECK(!close(gates[other][0]) && !close(reports[other][1]));}
            policy_child(directory,second,operation,id,ready[1],gates[id][0],reports[id][1]);}
        mixed_children[mixed_child_count++]=child;
    }
    CHECK(!close(ready[1]));unsigned seen=0;
    for(unsigned i=0;i<files;i++){struct policy_event e;transfer(ready[0],&e,sizeof(e),0);CHECK(e.id<files && !(seen&(1U<<e.id)));seen|=1U<<e.id;}
    for(unsigned id=0;id<files;id++)CHECK(!close(gates[id][0]) && !close(reports[id][1]));
    prepare=now()-prepare;start_observation();uint64_t start=now(),data_end=0;
    struct policy_round rounds[64];memset(rounds,0,sizeof(rounds));
    for(unsigned id=0;id<(active?1:files);id++)policy_release(gates[id][1]);
    if(periodic){
        for(unsigned round=0;round<16;round++){
            seen=0;
            for(unsigned i=0;i<files;i++){struct policy_event e;transfer(ready[0],&e,sizeof(e),0);CHECK(e.id<files && e.round==round && !(seen&(1U<<e.id)));seen|=1U<<e.id;
                rounds[round*files+e.id]=(struct policy_round){e.id,round,e.when,0,0};}
            uint64_t go=now();for(unsigned id=0;id<files;id++){rounds[round*files+id].go=go;policy_release(gates[id][1]);}
            seen=0;
            for(unsigned i=0;i<files;i++){struct policy_event e;transfer(ready[0],&e,sizeof(e),0);CHECK(e.id<files && e.round==round && !(seen&(1U<<e.id)));seen|=1U<<e.id;rounds[round*files+e.id].done=e.when;}
            for(unsigned id=0;id<files;id++)policy_release(gates[id][1]);
        }
    }else if(active){
        struct policy_event e;transfer(ready[0],&e,sizeof(e),0);CHECK(!e.id);
        rounds[0]=(struct policy_round){0,0,e.when,now(),0};
        for(unsigned id=0;id<files;id++)policy_release(gates[id][1]);
    }else{
        seen=0;for(unsigned i=0;i<files;i++){struct policy_event e;transfer(ready[0],&e,sizeof(e),0);CHECK(e.id<files && !(seen&(1U<<e.id)));seen|=1U<<e.id;if(e.when>data_end)data_end=e.when;}
        for(unsigned id=0;id<files;id++)policy_release(gates[id][1]);
    }
    struct policy_actor actors[MAX_FILES];struct policy_sample *samples[MAX_FILES]={0};
    for(unsigned id=0;id<files;id++){
        transfer(reports[id][0],&actors[id],sizeof(actors[id]),0);CHECK(actors[id].id==id && actors[id].count<=POLICY_SAMPLES);
        samples[id]=calloc(actors[id].count?actors[id].count:1,sizeof(*samples[id]));CHECK(samples[id]);
        transfer(reports[id][0],samples[id],actors[id].count*sizeof(*samples[id]),0);
        CHECK(!close(reports[id][0]) && !close(gates[id][1]));
    }
    for(unsigned id=0;id<files;id++){int status;CHECK(waitpid(mixed_children[id],&status,0)==mixed_children[id] && WIFEXITED(status) && !WEXITSTATUS(status));}
    mixed_child_count=0;CHECK(!close(ready[0]));uint64_t end=0;
    for(unsigned id=0;id<files;id++){
        if(actors[id].end>end)end=actors[id].end;
        if(periodic || active)for(unsigned i=0;i<actors[id].count;i++)
            if(samples[id][i].kind!=2 && samples[id][i].end>data_end)data_end=samples[id][i].end;
    }
    /* 传回样本和waitpid不属于工作量完成时间。 */
    CHECK(start<data_end && data_end<=end);stop_observation();
    uint64_t bytes=0,distinct=0;unsigned sync_calls=0;
    size_t page=sysconf(_SC_PAGESIZE);
    for(unsigned id=0;id<files;id++){
        bytes+=actors[id].bytes;
        for(unsigned j=0;j<actors[id].count;j++)if(samples[id][j].kind==2)sync_calls++;
        if(periodic || (active && !id) || domain){int fd=mixed_open(domain && id?second:directory,id,O_RDONLY);
            uint64_t length=domain?(!strcmp(operation,"domain-busy")?512*MIB:id?0:1024*MIB):active?16*MIB:MIB;
            CHECK(size_of(fd)==length);actors[id].checksum=policy_verify(fd,id,1,length);CHECK(!close(fd));distinct+=length;}
    }
    if(paired){
        int fd=mixed_open(directory,0,O_RDONLY);CHECK(size_of(fd)==4*MIB);uint64_t sum=0;
        unsigned epoch=8*MIB/1024/(4*MIB/page);
        for(uint64_t offset=0;offset<4*MIB;offset+=page){exact(fd,page,0,NULL);for(size_t j=0;j<page;j++){
            CHECK(buffer[j]==pattern(0,offset+j,j<2048?epoch:0));sum+=buffer[j];}}
        CHECK(read(fd,buffer,1)==0 && !close(fd));actors[0].checksum=sum;distinct=4*MIB/page*2048;
        struct statfs fs;int geometry=mixed_open(directory,0,O_RDONLY);CHECK(!fstatfs(geometry,&fs) && !close(geometry));
        fprintf(evidence,"IO POLICY_LAYOUT page_bytes=%zu block_bytes=1024 filesystem_block_bytes=%llu\n",page,(unsigned long long)fs.f_bsize);
    }
    policy_window(start,data_end,end,bytes,distinct,prepare,sync_calls);
    for(unsigned id=0;id<files;id++)policy_print(&actors[id],samples[id]);
    if(periodic)for(unsigned i=0;i<64;i++)fprintf(evidence,"IO POLICY_ROUND id=%u round=%u ready_ns=%llu go_ns=%llu done_ns=%llu\n",
        rounds[i].id,rounds[i].round,(unsigned long long)rounds[i].ready,(unsigned long long)rounds[i].go,(unsigned long long)rounds[i].done);
    if(active){
        for(unsigned i=0;i<actors[0].count;i++)if(samples[0] && samples[0][i].kind==2){rounds[0].done=samples[0][i].end;break;}
        fprintf(evidence,"IO POLICY_ROUND id=0 round=0 ready_ns=%llu go_ns=%llu done_ns=%llu\n",
            (unsigned long long)rounds[0].ready,(unsigned long long)rounds[0].go,(unsigned long long)rounds[0].done);
    }
    if(domain)for(unsigned id=0;id<2;id++)fprintf(evidence,"IO POLICY_DOMAIN id=%u device=%llu active=%u\n",id,(unsigned long long)domains[id].st_dev,id==0 || !strcmp(operation,"domain-busy"));
    for(unsigned id=0;id<files;id++)free(samples[id]);
    return policy_finish();
}

static void timed_out(int signal_number)
{
    (void)signal_number;
    for (unsigned i = 0; i < mixed_child_count; ++i) (void)kill(mixed_children[i], SIGKILL);
    const char message[] = "IO FAIL timeout\n";
    (void)write(2, message, sizeof(message) - 1);
    _exit(1);
}

int main(int argc, char **argv)
{
    signal(SIGALRM, timed_out);
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *config_path = argc > 1 ? argv[1] : "/io-config";
    char backend[16], operation[64], completion[16];
    FILE *config = fopen(config_path, "r"); CHECK(config);
    unsigned long long configured_bytes;
    unsigned timeout_seconds = 240;
    int fields = fscanf(config, "%15s %63s %15s %u %llu %u %u", backend, operation, completion,
                        &files, &configured_bytes, &request_bytes, &timeout_seconds);
    CHECK((fields == 6 || fields == 7) && timeout_seconds > 0 && timeout_seconds <= 3600 && fclose(config) == 0);
    alarm(timeout_seconds);
    file_bytes = configured_bytes;
    int parallel_policy=!strcmp(operation,"periodic-sync") || !strcmp(operation,"active-mixed") ||
        !strcmp(operation,"mmap-write") || !strcmp(operation,"pwrite-pair") || !strncmp(operation,"domain-",7);
    int policy=parallel_policy || !strncmp(operation,"writev-",7) || !strcmp(operation,"sync-write") || !strcmp(operation,"hot-write") ||
        !strcmp(operation,"sparse-write") || !strcmp(operation,"stream-write") || !strncmp(operation,"age-",4);
    if(policy){
        if(fields==6){timeout_seconds=120;alarm(timeout_seconds);}
        CHECK(timeout_seconds<=120);
        CHECK(!strcmp(backend,"ext4") && file_bytes && file_bytes<=1024*MIB);
        CHECK(request_bytes==1024 || request_bytes==4096 || request_bytes==65536);
        int n=snprintf(result_path,sizeof(result_path),"%s.results",config_path);CHECK(n>0 && (size_t)n<sizeof(result_path));
        choose_cpus();
        if(parallel_policy){
            int mounted=!strncmp(operation,"domain-",7) && argc<=3;
            if(mounted){CHECK(!mknod("/dev/vdb",S_IFBLK|0600,makedev(252,16)) || errno==EEXIST);
                CHECK(!mount("/dev/vdb","/data-second","ext4",0,NULL));}
            int status=policy_parallel(argc>2?argv[2]:"/data",argc>3?argv[3]:mounted?"/data-second":NULL,operation,completion);
            if(mounted)CHECK(!umount("/data-second"));
            return status;
        }
        return policy_single(argc>2?argv[2]:"/data",operation,completion);
    }
    int memory = !strcmp(backend, "tmpfs"), append = !strcmp(operation, "append");
    int writing = append || !strcmp(operation, "overwrite"), hot = !strcmp(operation, "hot-read");
    int mixed = !strcmp(operation, "mixed-read");
    int parallel = !strcmp(operation,"parallel-append"), range = !strcmp(operation,"range-modify");
    int sync_mode = !strcmp(completion, "fsync") ? 1 : !strcmp(completion, "fdatasync") ? 2 : 0;
    CHECK((memory || !strcmp(backend, "ext4")) && (writing || hot || mixed || parallel || range || !strcmp(operation, "cold-read") || !strcmp(operation, "read")));
    CHECK((sync_mode || !strcmp(completion, "cache")) && (writing || mixed || parallel || range || !sync_mode));
    CHECK(!mixed || (!memory && files == 4 && sync_mode == 1));
    CHECK(!(memory && !strcmp(operation, "cold-read")));
    CHECK((files == 1 || files == 2 || files == 4) && file_bytes > 0 && file_bytes <= 64U * 1024U * 1024U);
    CHECK(request_bytes == 1024 || request_bytes == 4096 || request_bytes == 65536);
    int n = snprintf(result_path, sizeof(result_path), "%s.results", config_path);
    CHECK(n > 0 && (size_t)n < sizeof(result_path));
    const char *directory = argc > 2 ? argv[2] : memory ? "/memory" : "/data";
    choose_cpus();
    unsigned cpu=pin_cpu(1);
    if (mixed || parallel) { CHECK(!parallel || (!memory && files==2 && sync_mode==1)); return mixed_read(directory,parallel); }
    if (range) { CHECK(!memory && files==1 && sync_mode==1); return range_modify(directory,cpu); }
    if (memory && argc <= 2) CHECK(mount("tmpfs", directory, "tmpfs", 0, "size=384m") == 0);
    struct result results[MAX_FILES] = {{0}};
    for (unsigned id = 0; id < files; ++id) {
        char path[1024]; n = snprintf(path, sizeof(path), "%s/file-%u", directory, id);
        CHECK(n > 0 && (size_t)n < sizeof(path));
        if (memory) {
            int setup = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600); CHECK(setup >= 0);
            if (!append) for (uint64_t offset = 0; offset < file_bytes;) {
                size_t count = file_bytes - offset;
                if (count > sizeof(buffer)) count = sizeof(buffer);
                fill(id, offset, count, 0); exact(setup, count, 1, NULL); offset += count;
            }
            CHECK(close(setup) == 0);
        }
        int flags = writing ? O_RDWR : O_RDONLY;
        if (append) flags |= O_APPEND;
        results[id].fd = open(path, flags); CHECK(results[id].fd >= 0);
        results[id].size_before = size_of(results[id].fd);
        CHECK(results[id].size_before == (append ? 0 : file_bytes));
        if (hot) (void)verify(results[id].fd, id, 0);
    }
    size_t per_file=(file_bytes+request_bytes-1)/request_bytes;
    struct io_interval *samples=calloc(files*per_file,sizeof(*samples));CHECK(samples);
    for (size_t offset=0;offset<files*per_file*sizeof(*samples);offset+=4096)
        ((volatile unsigned char *)samples)[offset]=0;
    start_observation();
    uint64_t start = now();
    /* 单任务逐请求轮转；每文件进展可核对，但不声称覆盖并发调度公平性。 */
    for (uint64_t offset = 0; offset < file_bytes;) {
        size_t count = file_bytes - offset;
        if (count > request_bytes) count = request_bytes;
        for (unsigned id = 0; id < files; ++id) {
            struct result *r = &results[id];
            if (!offset) r->start = now();
            if (writing) fill(id, offset, count, 1);
            struct io_interval *sample=&samples[id*per_file+offset/request_bytes];
            sample->start=now();
            exact(r->fd, count, writing, &r->calls);
            sample->end=now();
            r->bytes += count;
            if (!writing) r->checksum += check(id, offset, count, 0);
            if (writing && (offset == 0 || r->bytes == file_bytes / 4 || r->bytes == file_bytes / 2 || r->bytes == file_bytes * 3 / 4 || r->bytes == file_bytes)) {
                uint64_t size = size_of(r->fd);
                CHECK(size == (append ? r->bytes : file_bytes));
                CHECK(r->growth_count < 5); r->growth[r->growth_count++] = size;
            }
            if (r->bytes == file_bytes) r->end = now();
        }
        offset += count;
    }
    uint64_t data_end = now();
    unsigned sync_calls = 0;
    if (sync_mode) for (unsigned id = 0; id < files; ++id) {
        CHECK((sync_mode == 1 ? fsync(results[id].fd) : fdatasync(results[id].fd)) == 0);
        ++sync_calls; results[id].sync_end = now();
    }
    uint64_t end = sync_mode ? now() : data_end;
    stop_observation();
    uint64_t verify_start = now();
    for (unsigned id = 0; id < files; ++id) {
        struct result *r = &results[id];
        r->size_after = size_of(r->fd); CHECK(r->size_after == file_bytes && r->bytes == file_bytes);
        if (writing) r->checksum = verify(r->fd, id, 1);
        else CHECK(read(r->fd, buffer, 1) == 0);
    }
    uint64_t verify_end = now(), cleanup_start = verify_end;
    unsigned cleanup_sync_calls = 0;
    if (writing && !sync_mode) for (unsigned id = 0; id < files; ++id) {
        CHECK(fsync(results[id].fd) == 0); ++cleanup_sync_calls;
    }
    uint64_t cleanup_end = now();
    for (unsigned id = 0; id < files; ++id) CHECK(close(results[id].fd) == 0);
    fprintf(evidence, "IO WINDOW start_ns=%llu data_end_ns=%llu end_ns=%llu elapsed_ns=%llu data_ns=%llu sync_ns=%llu bytes=%llu sync_calls=%u verify_ns=%llu cleanup_sync_ns=%llu cleanup_sync_calls=%u complete=1\n",
            (unsigned long long)start, (unsigned long long)data_end, (unsigned long long)end,
            (unsigned long long)(end - start), (unsigned long long)(data_end - start), (unsigned long long)(end - data_end),
            (unsigned long long)(file_bytes * files), sync_calls,
            (unsigned long long)(verify_end - verify_start), (unsigned long long)(cleanup_end - cleanup_start), cleanup_sync_calls);
    for (unsigned id = 0; id < files; ++id) {
        const struct result *r = &results[id];
        fprintf(evidence,"IO AFFINITY role=%u cpu=%u\n",id,cpu);
        for (size_t sequence=0;sequence<per_file;sequence++) {
            const struct io_interval *sample=&samples[id*per_file+sequence];
            fprintf(evidence,"IO SAMPLE id=%u sequence=%llu start_ns=%llu end_ns=%llu\n",id,
                (unsigned long long)sequence,(unsigned long long)sample->start,(unsigned long long)sample->end);
        }
        fprintf(evidence, "IO FILE id=%u bytes=%llu calls=%llu start_ns=%llu end_ns=%llu sync_end_ns=%llu size_before=%llu size_after=%llu checksum=%llu complete=1\n",
                id, (unsigned long long)r->bytes, (unsigned long long)r->calls, (unsigned long long)r->start,
                (unsigned long long)r->end, (unsigned long long)r->sync_end, (unsigned long long)r->size_before,
                (unsigned long long)r->size_after, (unsigned long long)r->checksum);
        for (unsigned j = 0; j < r->growth_count; ++j)
            fprintf(evidence, "IO SIZE id=%u sample=%u bytes=%llu\n", id, j, (unsigned long long)r->growth[j]);
    }
    free(samples);
    if (memory && argc <= 2) CHECK(umount(directory) == 0);
    resources();
    fputs("IO PASS all\n", evidence);
    CHECK(fflush(evidence) == 0 && fsync(fileno(evidence)) == 0 && fclose(evidence) == 0);
    puts("IO PASS all");
    return 0;
}
