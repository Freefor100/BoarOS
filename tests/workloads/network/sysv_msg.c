#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef MSG_COPY
#define MSG_COPY 040000
#endif
#ifndef MSG_EXCEPT
#define MSG_EXCEPT 020000
#endif
#ifndef MSG_STAT_ANY
#define MSG_STAT_ANY 13
#endif
static unsigned cases;
static int live[32], count;
static void cleanup(void)
{
    for (int i = 0; i < count; i++)
        if (live[i] >= 0)
            msgctl(live[i], IPC_RMID, 0);
}
static void fail(const char *s, int l)
{
    int e = errno;
    cleanup();
    fprintf(stderr, "SYSV-MSG FAIL line=%d errno=%d %s\n", l, e, s);
    exit(1);
}
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            fail(#x, __LINE__);                                                                    \
    } while (0)
#define ERROR(x, e)                                                                                \
    do {                                                                                           \
        errno = 0;                                                                                 \
        CHECK((x) == -1 && errno == (e));                                                          \
    } while (0)
static void pass(const char *s)
{
    ++cases;
    printf("SYSV-MSG CASE %s PASS\n", s);
}
static int queue(void)
{
    int q = msgget(IPC_PRIVATE, 0600);
    CHECK(q >= 0 && count < 32);
    live[count++] = q;
    return q;
}
static void removeq(int q)
{
    CHECK(!msgctl(q, IPC_RMID, 0));
    for (int i = 0; i < count; i++)
        if (live[i] == q)
            live[i] = -1;
}
struct message {
    long type;
    char text[8192];
};
static void sendmsg(int q, long type, const char *s)
{
    struct message m = {.type = type};
    size_t n = strlen(s);
    memcpy(m.text, s, n);
    CHECK(!msgsnd(q, &m, n, IPC_NOWAIT));
}
static void recvmsg(int q, long type, int flags, long expected, const char *s)
{
    struct message m = {0};
    size_t n = strlen(s);
    CHECK(msgrcv(q, &m, sizeof(m.text), type, flags) == (ssize_t)n && m.type == expected &&
          !memcmp(m.text, s, n));
}
static struct msqid_ds statq(int q)
{
    struct msqid_ds d;
    CHECK(!msgctl(q, IPC_STAT, &d));
    return d;
}
static void abi(void)
{
#if defined(__riscv) || defined(__loongarch__)
    CHECK(SYS_msgget == 186 && SYS_msgctl == 187 && SYS_msgrcv == 188 && SYS_msgsnd == 189);
    CHECK(sizeof(struct msqid_ds) == 120 && sizeof(struct msginfo) == 32);
#endif
    int q = (int)syscall(SYS_msgget, IPC_PRIVATE, 0600);
    CHECK(q >= 0);
    live[count++] = q;
    struct message m = {.type = 9, .text = "raw"};
    CHECK(!syscall(SYS_msgsnd, q, &m, 3, IPC_NOWAIT));
    memset(&m, 0, sizeof(m));
    CHECK(syscall(SYS_msgrcv, q, &m, 3, 0, IPC_NOWAIT) == 3 && m.type == 9 &&
          !memcmp(m.text, "raw", 3));
    removeq(q);
    pass("native64-libc-raw");
}
static void creation(void)
{
    key_t key = (key_t)(0x4d000000U | ((unsigned)getpid() & 65535));
    int q = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    CHECK(q >= 0);
    live[count++] = q;
    CHECK(msgget(key, 0) == q);
    ERROR(msgget(key, IPC_CREAT | IPC_EXCL | 0600), EEXIST);
    removeq(q);
    ERROR(msgget(key, 0), ENOENT);
    ERROR(msgctl(q, IPC_STAT, 0), EINVAL);
    pass("key-generation-exclusive");
}
static void selection(void)
{
    int q = queue();
    sendmsg(q, 5, "five");
    sendmsg(q, 2, "two-a");
    sendmsg(q, 2, "two-b");
    sendmsg(q, 7, "seven");
    recvmsg(q, -6, IPC_NOWAIT, 2, "two-a");
    recvmsg(q, 2, IPC_NOWAIT, 2, "two-b");
    recvmsg(q, 5, IPC_NOWAIT | MSG_EXCEPT, 7, "seven");
    recvmsg(q, LONG_MIN, IPC_NOWAIT, 5, "five");
    struct message m;
    ERROR(msgrcv(q, &m, 8192, 0, IPC_NOWAIT), ENOMSG);
    sendmsg(q, 8, "first");
    sendmsg(q, 1, "second");
    recvmsg(q, 0, IPC_NOWAIT, 8, "first");
    recvmsg(q, 0, IPC_NOWAIT, 1, "second");
    removeq(q);
    pass("fifo-equal-except-negative-long-min");
}
static void truncation(void)
{
    int q = queue();
    sendmsg(q, 4, "abcdef");
    struct message m;
    ERROR(msgrcv(q, &m, 2, 0, IPC_NOWAIT), E2BIG);
    CHECK(statq(q).msg_qnum == 1);
    CHECK(msgrcv(q, &m, 2, 0, IPC_NOWAIT | MSG_NOERROR) == 2 && m.type == 4 &&
          !memcmp(m.text, "ab", 2));
    CHECK(!statq(q).msg_qnum);
    removeq(q);
    pass("e2big-preserves-noerror-consumes");
}
static void capacity(void)
{
    int q = queue();
    struct msqid_ds d = statq(q);
    d.msg_qbytes = 2;
    CHECK(!msgctl(q, IPC_SET, &d));
    struct message m = {.type = 1};
    CHECK(!msgsnd(q, &m, 0, IPC_NOWAIT) && !msgsnd(q, &m, 0, IPC_NOWAIT));
    ERROR(msgsnd(q, &m, 0, IPC_NOWAIT), EAGAIN);
    CHECK(statq(q).msg_qnum == 2 && !statq(q).msg_cbytes);
    CHECK(!msgrcv(q, &m, 0, 0, IPC_NOWAIT));
    sendmsg(q, 1, "ab");
    ERROR(msgsnd(q, &m, 1, IPC_NOWAIT), EAGAIN);
    CHECK(statq(q).msg_qnum == 2 && statq(q).msg_cbytes == 2);
    removeq(q);
    pass("byte-and-zero-message-count-capacity");
}
static void metadata(void)
{
    int q = queue();
    sendmsg(q, 3, "abc");
    struct msqid_ds d = statq(q);
    CHECK(d.msg_qbytes == 16384 && d.msg_qnum == 1 && d.msg_cbytes == 3 &&
          d.msg_lspid == getpid() && d.msg_stime > 0);
    recvmsg(q, 0, IPC_NOWAIT, 3, "abc");
    d = statq(q);
    CHECK(!d.msg_qnum && d.msg_lrpid == getpid() && d.msg_rtime > 0);
    struct msginfo i;
    int hi = msgctl(0, IPC_INFO, (void *)&i);
    CHECK(hi >= 0 && i.msgmax == 8192 && i.msgmni == 32000 && i.msgmnb == 16384);
    CHECK(msgctl(0, MSG_INFO, (void *)&i) >= 0 && i.msgpool >= 1);
    int found = 0;
    for (int n = 0; n <= hi; n++) {
        int r = msgctl(n, MSG_STAT_ANY, &d);
        if (r == q)
            found = 1;
        else
            CHECK(r >= 0 || errno == EINVAL);
    }
    CHECK(found);
    ERROR(msgctl(q, IPC_STAT | 0x100, &d), EINVAL);
    d = statq(q);
    d.msg_qbytes = UINT64_C(0x100000001);
    CHECK(!msgctl(q, IPC_SET, &d) && statq(q).msg_qbytes == 1);
    removeq(q);
    pass("stat-info-index-native-command");
}
static void copying(void)
{
    int q = queue();
    sendmsg(q, 5, "first");
    sendmsg(q, 2, "second");
    struct message m;
    memset(&m, 0, sizeof(m));
    CHECK(msgrcv(q, &m, 8192, 1, MSG_COPY | IPC_NOWAIT) == 6 && m.type == 2 &&
          !memcmp(m.text, "second", 6));
    CHECK(statq(q).msg_qnum == 2 && !statq(q).msg_rtime);
    ERROR(msgrcv(q, &m, 8192, -1, MSG_COPY | IPC_NOWAIT), ENOMSG);
    ERROR(msgrcv(q, &m, 8192, 2, MSG_COPY | IPC_NOWAIT), ENOMSG);
    ERROR(msgrcv(q, &m, 8192, 0, MSG_COPY), EINVAL);
    ERROR(msgrcv(q, &m, 8192, 0, MSG_COPY | MSG_EXCEPT | IPC_NOWAIT), EINVAL);
    ERROR(msgrcv(q, &m, 1, 0, MSG_COPY | IPC_NOWAIT), E2BIG);
    ERROR(msgrcv(q, &m, 1, 0, MSG_COPY | MSG_NOERROR | IPC_NOWAIT), EINVAL);
    CHECK(statq(q).msg_qnum == 2);
    removeq(q);
    pass("copy-ordinal-errors-and-no-mutation");
}
static void faults(void)
{
    int q = queue();
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *p = mmap(0, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED && !mprotect(p + page, page, PROT_NONE));
    struct message m = {.type = 1};
    ERROR(syscall(SYS_msgsnd, -1, p + page, 8193, 0), EFAULT);
    ERROR(msgsnd(q, &m, 8193, 0), EINVAL);
    *(long *)(p + page - 8) = 1;
    ERROR(msgsnd(q, p + page - 8, 1, 0), EFAULT);
    CHECK(!statq(q).msg_qnum);
    sendmsg(q, 2, "abc");
    ERROR(msgrcv(q, p + page, 8192, 0, MSG_COPY | IPC_NOWAIT), EFAULT);
    CHECK(statq(q).msg_qnum == 1);
    ERROR(msgrcv(q, p + page, 8192, 0, IPC_NOWAIT), EFAULT);
    CHECK(!statq(q).msg_qnum && statq(q).msg_lrpid == getpid());
    CHECK(!munmap(p, page * 2));
    removeq(q);
    pass("send-import-copy-prepare-receive-consumed-fault");
}
static void childwait(pid_t p)
{
    int s;
    CHECK(waitpid(p, &s, 0) == p && WIFEXITED(s) && !WEXITSTATUS(s));
}
static void numeric_permissions(void)
{
    CHECK(getuid() == 0 && geteuid() == 0);
    int q = queue(), index = -1;
    struct msqid_ds d = statq(q), entry;
    struct msginfo info;
    int hi = msgctl(0, IPC_INFO, (void *)&info);
    CHECK(hi >= 0);
    for (int i = 0; i <= hi; i++)
        if (msgctl(i, MSG_STAT_ANY, &entry) == q) {
            index = i;
            break;
        }
    CHECK(index >= 0 && d.msg_perm.uid == 0);
    int ready[2], proceed[2];
    CHECK(!pipe(ready) && !pipe(proceed));
    pid_t p = fork();
    CHECK(p >= 0);
    if (!p) {
        CHECK(!close(ready[0]) && !close(proceed[1]));
        CHECK(!setgroups(0, 0) && !setgid(1001) && !setuid(1001));
        CHECK(getuid() == 1001 && geteuid() == 1001 && getgid() == 1001 &&
              getegid() == 1001);
        struct message m = {.type = 101, .text = "numeric-user"};
        ERROR(msgsnd(q, &m, 12, IPC_NOWAIT), EACCES);
        ERROR(msgrcv(q, &m, sizeof(m.text), 0, IPC_NOWAIT), EACCES);
        ERROR(msgctl(q, IPC_STAT, &entry), EACCES);
        ERROR(msgctl(q, IPC_SET, &d), EPERM);
        ERROR(msgctl(q, IPC_RMID, 0), EPERM);
        CHECK(msgctl(index, MSG_STAT_ANY, &entry) == q && entry.msg_perm.uid == 0 &&
              (entry.msg_perm.mode & 0777) == 0600);
        char token = 'r';
        CHECK(write(ready[1], &token, 1) == 1);
        CHECK(read(proceed[0], &token, 1) == 1 && token == 'p');
        sendmsg(q, 101, "numeric-user");
        recvmsg(q, 0, IPC_NOWAIT, 101, "numeric-user");
        CHECK(!msgctl(q, IPC_STAT, &entry) && entry.msg_perm.uid == 0 &&
              (entry.msg_perm.mode & 0777) == 0666);
        ERROR(msgctl(q, IPC_SET, &d), EPERM);
        ERROR(msgctl(q, IPC_RMID, 0), EPERM);
        CHECK(!close(ready[1]) && !close(proceed[0]));
        _exit(0);
    }
    CHECK(!close(ready[1]) && !close(proceed[0]));
    char token;
    CHECK(read(ready[0], &token, 1) == 1 && token == 'r');
    CHECK(!statq(q).msg_qnum);
    d.msg_perm.mode = 0666;
    CHECK(!msgctl(q, IPC_SET, &d));
    token = 'p';
    CHECK(write(proceed[1], &token, 1) == 1);
    CHECK(!close(ready[0]) && !close(proceed[1]));
    childwait(p);
    CHECK(!statq(q).msg_qnum);
    removeq(q);
    pass("numeric-uid-permissions-and-stat-any");
}
static void process_exchange(void)
{
    int q = queue();
    pid_t p = fork();
    CHECK(p >= 0);
    if (!p) {
        recvmsg(q, 19, 0, 19, "child");
        sendmsg(q, 20, "parent");
        _exit(0);
    }
    sendmsg(q, 19, "child");
    recvmsg(q, 20, 0, 20, "parent");
    childwait(p);
    CHECK(!statq(q).msg_qnum);
    removeq(q);
    pass("fork-blocking-bidirectional-content");
}
static volatile sig_atomic_t caught;
static void handler(int s)
{
    caught = s;
}
static void signal_wait(void)
{
    int q = queue();
    struct sigaction a = {.sa_handler = handler, .sa_flags = SA_RESTART};
    sigemptyset(&a.sa_mask);
    CHECK(!sigaction(SIGALRM, &a, 0));
    struct message m;
    alarm(1);
    ERROR(msgrcv(q, &m, 8192, 0, 0), EINTR);
    CHECK(caught == SIGALRM);
    alarm(30);
    removeq(q);
    pass("signal-no-restart");
}

static void caller_cpus(void)
{
    cpu_set_t original, one, actual;
    CHECK(!sched_getaffinity(0, sizeof(original), &original));
    unsigned n = 0;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE && n < 4; cpu++)
        if (CPU_ISSET(cpu, &original)) {
            CPU_ZERO(&one);
            CPU_SET(cpu, &one);
            CHECK(!sched_setaffinity(0, sizeof(one), &one));
            CPU_ZERO(&actual);
            CHECK(!sched_getaffinity(0, sizeof(actual), &actual) &&
                  !memcmp(&one, &actual, sizeof(one)));
            int q = queue();
            sendmsg(q, 1, "caller");
            recvmsg(q, 0, IPC_NOWAIT, 1, "caller");
            removeq(q);
            printf("SYSV-MSG CPU index=%u affinity=%u send=1 receive=1 PASS\n", n, cpu);
            n++;
        }
    CHECK(n && !sched_setaffinity(0, sizeof(original), &original));
    pass("actual-caller-cpus");
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    atexit(cleanup);
    alarm(30);
    printf("SYSV-MSG BEGIN\n");
    abi();
    creation();
    selection();
    truncation();
    capacity();
    metadata();
    copying();
    faults();
    numeric_permissions();
    process_exchange();
    signal_wait();
    caller_cpus();
    cleanup();
    struct msginfo i;
    CHECK(msgctl(0, MSG_INFO, (void *)&i) >= 0);
    printf("SYSV-MSG RESOURCE queues=%d messages=%d bytes=%d\n", i.msgpool, i.msgmap, i.msgtql);
    printf("SYSV-MSG END %u\n", cases);
    alarm(0);
    if (getpid() == 1) {
        sync();
        reboot(RB_POWER_OFF);
    }
    return 0;
}
