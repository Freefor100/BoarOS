#define COST_END_WAIT_ASYNC 1
#include "common.h"
#include <sched.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <termios.h>
#define SWITCHES 8192U
#define CREATES 1024U
static void drain(void)
{ CHECK(fflush(stdout) == 0); CHECK(tcdrain(STDOUT_FILENO) == 0); }
static void reap(pid_t child)
{ int status; CHECK(waitpid(child, &status, 0) == child && status == 0); }
static void one_child(void)
{ pid_t child = fork(); CHECK(child >= 0); if (!child) _exit(0); reap(child); CHECK(sched_yield() == 0); }
static void switching(int pipes)
{
    int gate[2], done[2]; CHECK(pipe(gate) == 0 && pipe(done) == 0);
    volatile unsigned *progress = mmap(0, 4096, PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(progress != MAP_FAILED); *progress = 0;
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        char byte; CHECK(read(gate[0], &byte, 1) == 1);
        for (unsigned i = 0; i < SWITCHES; i++) {
            if (pipes) { CHECK(write(done[1], "p", 1) == 1); CHECK(read(gate[0], &byte, 1) == 1); }
            else CHECK(sched_yield() == 0);
            *progress = i + 1;
        }
        CHECK(write(done[1], "d", 1) == 1); _exit(0);
    }
    const char *name = pipes ? "scheduler-wake" : "scheduler-yield";
    drain(); cost_begin(); CHECK(write(gate[1], "s", 1) == 1);
    uint64_t checksum = 0;
    for (unsigned i = 0; i < SWITCHES; i++) {
        if (pipes) { char byte; CHECK(read(done[0], &byte, 1) == 1 && byte == 'p'); CHECK(write(gate[1], "a", 1) == 1); checksum += (unsigned char)byte; }
        else CHECK(sched_yield() == 0);
    }
    char byte; CHECK(read(done[0], &byte, 1) == 1 && byte == 'd'); reap(child);
    CHECK(*progress == SWITCHES); CHECK(sched_yield() == 0);
    cost_end(name, 0, 0, 0, 0);
    printf("COST SCHED WORK %s %u %llu\n", name, SWITCHES,
           (unsigned long long)(pipes ? checksum : *progress));
    CHECK(munmap((void *)progress, 4096) == 0);
    CHECK(!close(gate[0]) && !close(gate[1]) && !close(done[0]) && !close(done[1]));
}
int main(void)
{
    cost_init(); one_child(); switching(0); switching(1);
    drain(); cost_begin();
    for (unsigned i = 0; i < CREATES; i++) one_child();
    cost_end("scheduler-create", 0, 0, 0, 0);
    printf("COST SCHED WORK scheduler-create %u %u\n", CREATES, CREATES);
    puts("COST PASS scheduler"); drain(); return 0;
}
