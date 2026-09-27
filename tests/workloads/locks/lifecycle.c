#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *path;
static int held_fd, waiting_fd, ready_pipe[2], done_pipe[2];
static int wait_result;
static int wait_error;
static int wait_command;
static off_t wait_start;

static void check(int condition, const char *step)
{
    if (condition) return;
    fprintf(stderr, "record lock lifecycle %s errno=%d\n", step, errno);
    exit(1);
}

static struct flock write_lock(off_t start)
{
    return (struct flock){ .l_type = F_WRLCK, .l_whence = SEEK_SET,
                           .l_start = start, .l_len = 1 };
}

static void *waiter(void *argument)
{
    (void)argument;
    struct flock lock = write_lock(wait_start);
    if (write(ready_pipe[1], "r", 1) != 1) return (void *)1;
    wait_result = fcntl(waiting_fd, wait_command, &lock);
    wait_error = wait_result ? errno : 0;
    if (write(done_pipe[1], "d", 1) != 1) return (void *)2;
    return 0;
}

static void *traditional_worker(void *argument)
{
    int fd = *(int *)argument;
    struct flock lock = write_lock(10);
    return fcntl(fd, F_SETLK, &lock) == 0 ? 0 : (void *)1;
}

static void shared_table_test(void)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "shared open");
    struct flock lock = write_lock(10);
    check(fcntl(fd, F_SETLK, &lock) == 0, "main traditional lock");
    pthread_t thread;
    void *thread_result = 0;
    /* The same shared files_struct owner may replace its own lock. */
    check(pthread_create(&thread, 0, traditional_worker, &fd) == 0,
          "thread create");
    check(pthread_join(thread, &thread_result) == 0 && !thread_result,
          "shared traditional owner");
    int other = open(path, O_RDWR);
    check(other >= 0, "observer open");
    lock = (struct flock){ .l_type = F_RDLCK, .l_whence = SEEK_SET,
                           .l_start = 10, .l_len = 1 };
    check(fcntl(other, F_OFD_GETLK, &lock) == 0 &&
          lock.l_type == F_WRLCK, "traditional visible to OFD");
    pid_t child = fork();
    check(child >= 0, "fork");
    if (!child) {
        struct flock child_lock = write_lock(10);
        _exit(fcntl(fd, F_SETLK, &child_lock) < 0 && errno == EAGAIN ? 0 : 2);
    }
    int status;
    check(waitpid(child, &status, 0) == child &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "fork does not inherit traditional owner");
    check(close(other) == 0, "close any inode fd");
    other = open(path, O_RDWR);
    check(other >= 0, "reopen observer");
    lock = (struct flock){ .l_type = F_RDLCK, .l_whence = SEEK_SET,
                           .l_start = 10, .l_len = 1 };
    check(fcntl(other, F_OFD_GETLK, &lock) == 0 &&
          lock.l_type == F_UNLCK, "any close releases traditional lock");
    check(close(fd) == 0 && close(other) == 0, "shared close");
}

static void reused_fd_test(void)
{
    held_fd = open(path, O_RDWR);
    waiting_fd = open(path, O_RDWR);
    check(held_fd >= 0 && waiting_fd >= 0, "reuse opens");
    struct flock lock = write_lock(0);
    check(fcntl(held_fd, F_OFD_SETLK, &lock) == 0, "holder OFD lock");
    check(pipe(ready_pipe) == 0 && pipe(done_pipe) == 0, "reuse pipes");
    wait_start = 0;
    wait_command = F_OFD_SETLKW;
    pthread_t thread;
    check(pthread_create(&thread, 0, waiter, 0) == 0, "waiter create");
    char byte;
    check(read(ready_pipe[0], &byte, 1) == 1, "waiter ready");
    struct pollfd pollfd = { .fd = done_pipe[0], .events = POLLIN };
    check(poll(&pollfd, 1, 100) == 0, "waiter blocked");
    int old_number = waiting_fd;
    check(close(waiting_fd) == 0, "close blocked fd");
    waiting_fd = open(path, O_RDWR);
    check(waiting_fd == old_number, "fd reused");
    check(close(held_fd) == 0, "holder released");
    check(read(done_pipe[0], &byte, 1) == 1, "waiter completes");
    check(pthread_join(thread, 0) == 0 && wait_result == 0,
          "pinned old OFD completes");
    check(close(waiting_fd) == 0, "reused close");
    for (int side = 0; side < 2; ++side)
        check(close(ready_pipe[side]) == 0 &&
              close(done_pipe[side]) == 0, "pipe close");
}

static void reused_traditional_fd_test(void)
{
    held_fd = open(path, O_RDWR);
    waiting_fd = open(path, O_RDWR);
    check(held_fd >= 0 && waiting_fd >= 0, "traditional reuse opens");
    struct flock lock = write_lock(30);
    check(fcntl(held_fd, F_OFD_SETLK, &lock) == 0,
          "traditional reuse blocker");
    check(pipe(ready_pipe) == 0 && pipe(done_pipe) == 0,
          "traditional reuse pipes");
    wait_start = 30;
    wait_command = F_SETLKW;
    pthread_t thread;
    check(pthread_create(&thread, 0, waiter, 0) == 0,
          "traditional waiter create");
    char byte;
    check(read(ready_pipe[0], &byte, 1) == 1,
          "traditional waiter ready");
    struct pollfd pollfd = { .fd = done_pipe[0], .events = POLLIN };
    check(poll(&pollfd, 1, 100) == 0, "traditional waiter blocked");
    int old_number = waiting_fd;
    check(close(waiting_fd) == 0, "traditional waiting fd close");
    waiting_fd = open(path, O_RDWR);
    check(waiting_fd == old_number, "traditional fd reuse");
    check(close(held_fd) == 0, "traditional blocker release");
    check(read(done_pipe[0], &byte, 1) == 1 &&
          pthread_join(thread, 0) == 0, "traditional wait completion");
    printf("BoarOS: traditional fd reuse result=%d errno=%d\n",
           wait_result, wait_error);
    check(close(waiting_fd) == 0, "traditional reused close");
    for (int side = 0; side < 2; ++side)
        check(close(ready_pipe[side]) == 0 &&
              close(done_pipe[side]) == 0, "traditional pipe close");
}

static void forced_exit_test(void)
{
    int holder = open(path, O_RDWR);
    check(holder >= 0, "forced holder open");
    struct flock lock = write_lock(20);
    check(fcntl(holder, F_OFD_SETLK, &lock) == 0, "forced holder lock");
    wait_start = 20;
    pid_t child = fork();
    check(child >= 0, "forced fork");
    if (!child) {
        waiting_fd = open(path, O_RDWR);
        if (waiting_fd < 0 || pipe(ready_pipe) || pipe(done_pipe)) _exit(2);
        wait_command = F_OFD_SETLKW;
        pthread_t thread;
        if (pthread_create(&thread, 0, waiter, 0)) _exit(3);
        char byte;
        if (read(ready_pipe[0], &byte, 1) != 1) _exit(4);
        struct pollfd pollfd = { .fd = done_pipe[0], .events = POLLIN };
        if (poll(&pollfd, 1, 100) != 0) _exit(5);
        _exit(0); /* exit_group while another thread blocks in fcntl */
    }
    int status;
    check(waitpid(child, &status, 0) == child &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "forced child reaped");
    check(close(holder) == 0, "forced holder close");
}

int main(void)
{
    path = getenv("LOCK_TEST_PATH");
    if (!path) path = "/lockdata";
    shared_table_test();
    reused_fd_test();
    reused_traditional_fd_test();
    forced_exit_test();
    puts("BoarOS: record lock lifecycle passed");
    return 42;
}
