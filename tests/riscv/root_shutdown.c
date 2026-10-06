#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int ready_fd, block_fd;
static pthread_t leader;

static void *blocked(void *join_leader)
{
    if (join_leader) {
        if (pthread_join(leader, NULL)) _exit(80);
        if (write(ready_fd, "r", 1) != 1) _exit(81);
    }
    char byte;
    for (;;) (void)read(block_fd, &byte, 1);
}

int main(void)
{
    char mode[16] = {0};
    int fd = open("/mode", O_RDONLY);
    if (fd < 0 || read(fd, mode, sizeof(mode) - 1) <= 0) return 70;
    close(fd);
    int ready[2], hold[2];
    if (pipe(ready) || pipe(hold)) return 71;
    pid_t child = fork();
    if (child < 0) return 72;
    if (!child) {
        close(ready[0]);
        if (setsid() < 0 || open("/data", O_RDONLY) < 0) _exit(73);
        int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0 || listen(socket_fd, 1)) _exit(74);
        ready_fd = ready[1]; block_fd = hold[0];
        /* 保留写端，使PID 1退出不会通过EOF自然结束这个后台owner。 */
        if (!strcmp(mode, "threads")) {
            pthread_t workers[2]; leader = pthread_self();
            if (pthread_create(&workers[0], NULL, blocked, NULL) ||
                pthread_create(&workers[1], NULL, blocked, (void *)1)) _exit(75);
            pthread_exit(NULL);
        }
        if (write(ready_fd, "r", 1) != 1) _exit(76);
        if (!strcmp(mode, "stopped")) raise(SIGSTOP);
        if (!strcmp(mode, "forking")) {
            for (unsigned i = 0; i < 8; i++) {
                pid_t descendant = fork();
                if (descendant < 0) _exit(77);
                if (!descendant) blocked(NULL);
            }
        }
        blocked(NULL);
    }
    close(ready[1]);
    char byte;
    if (read(ready[0], &byte, 1) != 1) return 78;
    if (!strcmp(mode, "stopped")) {
        int status;
        if (waitpid(child, &status, WUNTRACED) != child || !WIFSTOPPED(status))
            return 79;
    }
    printf("INIT SHUTDOWN READY %s\n", mode);
    fflush(stdout);
    return 37;
}
