#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum { GET_ID=0, JOIN=1, UPDATE=2, REVOKE=3, CHOWN=4, SETPERM=5, DESCRIBE=6,
    CLEAR=7, LINK=8, UNLINK=9, SEARCH=10, READ=11, DEFAULT=14, TIMEOUT=15,
    INVALIDATE=21 };
enum { THREAD=-1, PROCESS=-2, SESSION=-3, UID=-4, UID_SESSION=-5 };
#define KC(action, a, b, c, d) syscall(SYS_keyctl, (long)(action), (long)(a), \
    (long)(b), (long)(c), (long)(d))
#define ID(scope, create) KC(GET_ID, scope, create, 0, 0)
#define ADD(type, desc, buf, bytes, ring) syscall(SYS_add_key, type, desc, buf, \
    (long)(bytes), (long)(ring))
#define REQ(type, desc, callout, ring) syscall(SYS_request_key, type, desc, callout, (long)(ring))
#define EXPECT(cond) do { if (!(cond)) { \
    fprintf(stderr, "keys FAIL line=%d %s errno=%d\n", __LINE__, #cond, errno); exit(1); \
} } while (0)
#define ERR(expr, code) EXPECT((expr) == -1 && errno == (code))

static long thread_ring, process_ring, session_ring;
static const char sample[] = "sample";
static const char replacement[] = "data";

static void *thread_fn(void *unused)
{
    (void)unused;
    long thread = ID(THREAD, 0);
    EXPECT(thread > 0 && thread != thread_ring);
    EXPECT(ID(PROCESS, 0) == process_ring);
    EXPECT(ID(SESSION, 0) == session_ring);
    ERR(REQ("user", "t-only", NULL, 0), ENOKEY);
    EXPECT(REQ("user", "p-shared", NULL, 0) > 0);
    return NULL;
}

static void fork_exec(void)
{
    pid_t pid = fork();
    EXPECT(pid >= 0);
    if (!pid) {
        ERR(ID(THREAD, 0), ENOKEY);
        ERR(ID(PROCESS, 0), ENOKEY);
        EXPECT(ID(SESSION, 0) == session_ring);
        ERR(REQ("user", "p-shared", NULL, 0), ENOKEY);
        EXPECT(REQ("user", "s-shared", NULL, 0) > 0);
        EXPECT(ID(THREAD, 1) > 0);
        EXPECT(ID(PROCESS, 1) > 0);
        char session[20];
        snprintf(session, sizeof(session), "%ld", session_ring);
        execl("/init", "/init", session, (char *)NULL);
        EXPECT(0);
    }
    int status;
    EXPECT(waitpid(pid, &status, 0) == pid && status == 0);
}

static void quota(void)
{
    pid_t pid = fork();
    EXPECT(pid >= 0);
    if (!pid) {
        EXPECT(setuid(13242) == 0);
        long user = ID(UID, 0), fallback = ID(UID_SESSION, 0);
        EXPECT(user > 0 && fallback > 0 && fallback != user);
        long thread = ID(THREAD, 1);
        EXPECT(thread > 0);
        char desc[40];
        unsigned i;
        for (i = 0; i < 197; ++i) {
            snprintf(desc, sizeof(desc), "quota-%03u", i);
            EXPECT(ADD("user", desc, sample, sizeof(sample), thread) > 0);
        }
        ERR(ADD("user", "quota-last", sample, sizeof(sample), thread), EDQUOT);
        EXPECT(KC(CLEAR, thread, 0, 0, 0) == 0);
        EXPECT(ADD("user", "quota-last", sample, sizeof(sample), thread) > 0);
        _exit(0);
    }
    int status;
    EXPECT(waitpid(pid, &status, 0) == pid && status == 0);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        EXPECT(ID(SESSION, 0) == strtol(argv[1], NULL, 10));
        ERR(ID(THREAD, 0), ENOKEY);
        ERR(ID(PROCESS, 0), ENOKEY);
        EXPECT(REQ("user", "s-shared", NULL, 0) > 0);
        return 0;
    }
    ERR(ID(THREAD, 0), ENOKEY);
    ERR(ID(PROCESS, 0), ENOKEY);
    long default_session = ID(SESSION, 0);
    EXPECT(default_session == ID(UID_SESSION, 0));
    EXPECT(ID(SESSION, 1) != default_session);
    session_ring = KC(JOIN, 0, 0, 0, 0);
    thread_ring = ID(THREAD, 1);
    process_ring = ID(PROCESS, 1);
    EXPECT(thread_ring > 0 && process_ring > 0 && session_ring > 0);
    EXPECT(ADD("user", "t-only", sample, sizeof(sample), thread_ring) > 0);
    EXPECT(ADD("user", "p-shared", sample, sizeof(sample), process_ring) > 0);
    EXPECT(ADD("user", "s-shared", sample, sizeof(sample), session_ring) > 0);
    pthread_t thread;
    EXPECT(pthread_create(&thread, NULL, thread_fn, NULL) == 0);
    EXPECT(pthread_join(thread, NULL) == 0);
    fork_exec();
    quota();

    EXPECT(KC(DEFAULT, 1, 0, 0, 0) == 0);
    EXPECT(ID(THREAD, 0) == thread_ring);
    EXPECT(KC(DEFAULT, -1, 0, 0, 0) == 1);
    ERR(REQ("user", "pending-1", NULL, 0), ENOKEY);
    ERR(REQ("user", "pending-1", "construct", 0), ENOKEY);
    int32_t ids[4];
    EXPECT(KC(READ, thread_ring, ids, sizeof(ids), 0) == 2 * sizeof(ids[0]));
    long negative = 0;
    char desc[100];
    for (unsigned i = 0; i < 2; ++i) {
        EXPECT(KC(DESCRIBE, ids[i], desc, sizeof(desc), 0) > 0);
        if (strstr(desc, ";pending-1")) negative = ids[i];
    }
    EXPECT(negative > 0);
    char bytes[64];
    ERR(KC(READ, negative, bytes, sizeof(bytes), 0), ENOKEY);
    EXPECT(KC(UPDATE, negative, replacement, sizeof(replacement), 0) == 0);
    EXPECT(REQ("user", "pending-1", NULL, 0) == negative);
    EXPECT(KC(READ, negative, bytes, sizeof(bytes), 0) == sizeof(replacement));
    EXPECT(memcmp(bytes, replacement, sizeof(replacement)) == 0);
    EXPECT(ADD("user", "pending-1", sample, sizeof(sample), thread_ring) == negative);
    EXPECT(KC(READ, negative, bytes, sizeof(bytes), 0) == sizeof(sample));
    EXPECT(memcmp(bytes, sample, sizeof(sample)) == 0);
    EXPECT(KC(READ, negative, (char *)1, 1, 0) == sizeof(sample));
    ERR(KC(READ, negative, (char *)1, sizeof(sample), 0), EFAULT);
    EXPECT(REQ("user", "pending-1", NULL, process_ring) == negative);
    EXPECT(KC(UNLINK, negative, thread_ring, 0, 0) == 0);
    EXPECT(REQ("user", "pending-1", NULL, 0) == negative);
    EXPECT(KC(UNLINK, negative, process_ring, 0, 0) == 0);
    ERR(REQ("user", "pending-1", NULL, 0), ENOKEY);

    long a = ADD("keyring", "a", NULL, 0, thread_ring);
    long b = ADD("keyring", "b", NULL, 0, a);
    EXPECT(a > 0 && b > 0);
    ERR(KC(LINK, a, b, 0, 0), EDEADLK);
    long logon = ADD("logon", "service:user", sample, sizeof(sample), b);
    EXPECT(logon > 0 && KC(SEARCH, thread_ring, "logon", "service:user", 0) == logon);
    ERR(KC(READ, logon, bytes, sizeof(bytes), 0), EOPNOTSUPP);
    ERR(ADD("logon", "unqualified", sample, sizeof(sample), b), EINVAL);
    ERR(ADD("keyring", ".hidden", NULL, 0, b), EPERM);

    long expires = ADD("user", "expires", sample, sizeof(sample), b);
    EXPECT(expires > 0 && KC(TIMEOUT, expires, 1, 0, 0) == 0);
    EXPECT(sleep(2) == 0);
    ERR(KC(READ, expires, bytes, sizeof(bytes), 0), EKEYEXPIRED);
    long revoked = ADD("user", "revoke", sample, sizeof(sample), b);
    EXPECT(revoked > 0 && KC(REVOKE, revoked, 0, 0, 0) == 0);
    ERR(KC(READ, revoked, bytes, sizeof(bytes), 0), EKEYREVOKED);
    EXPECT(KC(UNLINK, revoked, b, 0, 0) == 0);
    ERR(KC(READ, revoked, bytes, sizeof(bytes), 0), ENOKEY);

    long hidden = ADD("user", "hide", sample, sizeof(sample), b);
    EXPECT(hidden > 0 && KC(SETPERM, hidden, 0, 0, 0) == 0);
    ERR(KC(UPDATE, hidden, replacement, sizeof(replacement), 0), EACCES);
    ERR(KC(DESCRIBE, hidden, desc, sizeof(desc), 0), EACCES);
    EXPECT(KC(UNLINK, hidden, b, 0, 0) == 0);
    unsigned char caps[8];
    memset(caps, 255, sizeof(caps));
    EXPECT(KC(31, caps, sizeof(caps), 0, 0) == 2);
    EXPECT(caps[0] == 17);
    for (unsigned i = 1; i < sizeof(caps); ++i) EXPECT(caps[i] == 0);
    long possessed = ADD("user", "possession-only", sample, sizeof(sample), session_ring);
    EXPECT(possessed > 0);
    EXPECT(KC(SETPERM, possessed, 0x0a000000, 0, 0) == 0);
    EXPECT(KC(READ, possessed, bytes, sizeof(bytes), 0) == sizeof(sample));
    EXPECT(KC(INVALIDATE, session_ring, 0, 0, 0) == 0);
    ERR(KC(READ, possessed, bytes, sizeof(bytes), 0), EACCES);
    puts("keys lifecycle PASS");
    return 0;
}
