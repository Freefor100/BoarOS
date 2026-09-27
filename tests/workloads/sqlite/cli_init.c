#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void run(const char *program)
{
    int output[2];
    if (pipe(output)) exit(1);
    pid_t child = fork();
    if (child < 0) exit(1);
    if (!child) {
        close(output[0]);
        if (dup2(output[1], 1) != 1) _exit(3);
        close(output[1]);
        execl(program, program, "/boaros.db",
              "PRAGMA journal_mode=DELETE; PRAGMA locking_mode=NORMAL;"
              " PRAGMA mmap_size=0; PRAGMA synchronous=EXTRA;"
              " PRAGMA synchronous;"
              " SELECT sqlite_version(),"
              "sqlite_compileoption_used('THREADSAFE=1');"
              " PRAGMA integrity_check;",
              (char *)0);
        _exit(2);
    }
    close(output[1]);
    char actual[256];
    size_t used = 0;
    ssize_t count;
    while ((count = read(output[0], actual + used,
                         sizeof(actual) - used - 1)) > 0) {
        used += (size_t)count;
        if (used == sizeof(actual) - 1) break;
    }
    close(output[0]);
    actual[used] = 0;
    static const char expected[] =
        "delete\nnormal\n0\n3\n3.53.4|1\nok\n";
    int status = 0;
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        count < 0 || strcmp(actual, expected)) {
        fprintf(stderr, "SQLite CLI %s failed status=%d output=%s\n",
                program, status, actual);
        exit(1);
    }
}

int main(void)
{
    run("/sqlite3-static");
    run("/sqlite3-dynamic");
    puts("BoarOS: SQLite CLI static and dynamic passed");
    return 42;
}
