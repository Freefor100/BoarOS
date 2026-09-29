/* The same unmodified init runs on the fixed Linux kernel and BoarOS. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

static const char *const names[] = {
    "preprocess", "compile", "assemble", "link", "run"
};

static int tmpfs_work;

static int copy_from(int source_directory, const char *name,
                      const char *destination_directory)
{
    int input = openat(source_directory, name, O_RDONLY);
    if (input < 0) return -1;
    char path[256];
    if (snprintf(path, sizeof(path), "%s/%s", destination_directory, name) >= (int)sizeof(path)) {
        close(input); errno = ENAMETOOLONG; return -1;
    }
    struct stat status;
    if (fstat(input, &status)) { close(input); return -1; }
    int output = open(path, O_WRONLY | O_CREAT | O_EXCL, status.st_mode & 0777);
    if (output < 0) { close(input); return -1; }
    char buffer[8192];
    ssize_t n;
    while ((n = read(input, buffer, sizeof(buffer))) > 0) {
        ssize_t offset = 0;
        while (offset < n) {
            ssize_t written = write(output, buffer + offset, (size_t)(n - offset));
            if (written <= 0) { close(input); close(output); return -1; }
            offset += written;
        }
    }
    int failed = n < 0 || fsync(output);
    if (close(input)) failed = 1;
    if (close(output)) failed = 1;
    return failed ? -1 : 0;
}

static int prepare_tmpfs(void)
{
    int source = open("/work", O_RDONLY | O_DIRECTORY);
    if (source < 0) return -1;
    if (mount("none", "/work", "tmpfs", 0, "size=67108864,nr_inodes=1024")) {
        close(source); return -1;
    }
    /* The old directory fd owns the fixture inputs while tmpfs hides /work. */
    int failed = copy_from(source, "program.c", "/work") ||
                 copy_from(source, "tools.conf", "/work");
    if (close(source)) failed = 1;
    struct statfs filesystem;
    if (statfs("/work", &filesystem) || filesystem.f_type != 0x01021994 ||
        chdir("/work")) failed = 1;
    if (!failed) puts("TOOLCHAIN tmpfs work directory active");
    return failed ? -1 : 0;
}

static int export_tmpfs(void)
{
    const char *const artifacts[] = {"program.i", "program.s", "program.o",
        "program", "output.txt", "stages.tsv"};
    int source = open("/work", O_RDONLY | O_DIRECTORY);
    if (source < 0) return -1;
    for (size_t i = 0; i < sizeof(artifacts) / sizeof(artifacts[0]); i++) {
        struct stat status;
        if (fstatat(source, artifacts[i], &status, 0)) {
            if (errno == ENOENT) continue;
            close(source); return -1;
        }
        if (copy_from(source, artifacts[i], "/offline-evidence")) {
            close(source); return -1;
        }
    }
    if (close(source)) return -1;
    int directory = open("/offline-evidence", O_RDONLY | O_DIRECTORY);
    if (directory < 0) return -1;
    int failed = fsync(directory);
    if (close(directory)) failed = 1;
    if (chdir("/") || umount("/work")) failed = 1;
    if (!failed) puts("TOOLCHAIN tmpfs artifacts copied to root evidence; tmpfs is volatile");
    return failed ? -1 : 0;
}

static int read_tools(char *compiler, size_t compiler_size,
                      char *assembler, size_t assembler_size)
{
    FILE *config = fopen("/work/tools.conf", "r");
    if (!config) return -1;
    char *paths[] = { compiler, assembler };
    size_t sizes[] = { compiler_size, assembler_size };
    for (int i = 0; i < 2; ++i) {
        if (!fgets(paths[i], sizes[i], config)) {
            fclose(config);
            return -1;
        }
        size_t length = strlen(paths[i]);
        if (length < 2 || paths[i][0] != '/' ||
            paths[i][length - 1] != '\n') {
            fclose(config);
            return -1;
        }
        paths[i][length - 1] = 0;
    }
    int extra = fgetc(config);
    fclose(config);
    return extra == EOF ? 0 : -1;
}

static void record(FILE *results, const char *name, const char *kind, int code)
{
    if (fprintf(results, "%s\t%s\t%d\n", name, kind, code) < 0 ||
        fflush(results) || fsync(fileno(results))) {
        _exit(91);
    }
    int directory = open("/work", O_RDONLY | O_DIRECTORY);
    if (directory < 0 || fsync(directory) || close(directory)) _exit(91);
    printf("TOOLCHAIN stage=%s kind=%s code=%d\n", name, kind, code);
    fflush(stdout);
}

static int persist(const char *path)
{
    int descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return -1;
    if (fsync(descriptor)) {
        int error = errno;
        close(descriptor);
        errno = error;
        return -1;
    }
    if (close(descriptor)) return -1;
    int directory = open("/work", O_RDONLY | O_DIRECTORY);
    if (directory < 0) return -1;
    if (fsync(directory)) {
        int error = errno;
        close(directory);
        errno = error;
        return -1;
    }
    return close(directory);
}

static int launch(char *const arguments[], const char *output,
                  const char **kind, int *code)
{
    int error_pipe[2];
    if (pipe(error_pipe)) {
        *kind = "pipe";
        *code = errno;
        return -1;
    }
    if (fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) < 0) {
        *kind = "fcntl";
        *code = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        *kind = "fork";
        *code = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        return -1;
    }
    if (child == 0) {
        close(error_pipe[0]);
        if (output) {
            int fd = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) {
                int error = errno;
                (void)write(error_pipe[1], &error, sizeof(error));
                _exit(127);
            }
            close(fd);
        }
        char *const environment[] = {
            "PATH=/usr/bin:/bin", "HOME=/",
            tmpfs_work ? "TMPDIR=/work" : "TMPDIR=/tmp", "LC_ALL=C", 0
        };
        execve(arguments[0], arguments, environment);
        int error = errno;
        (void)write(error_pipe[1], &error, sizeof(error));
        _exit(127);
    }
    close(error_pipe[1]);
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    int child_error = 0;
    ssize_t read_size = read(error_pipe[0], &child_error, sizeof(child_error));
    close(error_pipe[0]);
    if (waited != child) {
        *kind = "wait";
        *code = errno;
    } else if (read_size == sizeof(child_error)) {
        *kind = "exec";
        *code = child_error;
    } else if (read_size != 0) {
        *kind = "protocol";
        *code = (int)read_size;
    } else if (WIFEXITED(status)) {
        *kind = "exit";
        *code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        *kind = "signal";
        *code = WTERMSIG(status);
    } else {
        *kind = "wait-status";
        *code = status;
    }
    return strcmp(*kind, "exit") == 0 && *code == 0 ? 0 : -1;
}

int main(void)
{
    char compiler[256], assembler[256];
    if (read_tools(compiler, sizeof(compiler),
                   assembler, sizeof(assembler))) {
        fprintf(stderr, "TOOLCHAIN invalid /work/tools.conf errno=%d\n", errno);
        return 90;
    }
    tmpfs_work = access("/work/tmpfs.mode", F_OK) == 0;
    if (tmpfs_work && prepare_tmpfs()) {
        fprintf(stderr, "TOOLCHAIN tmpfs setup failed errno=%d\n", errno);
        return 93;
    }
    FILE *results = fopen("/work/stages.tsv", "w");
    if (!results) return 91;
    char *preprocess[] = { compiler, "-E", "/work/program.c", "-o",
                           "/work/program.i", 0 };
    char *compile[] = { compiler, "-S", "-x", "cpp-output", "-O2",
                        "/work/program.i", "-o", "/work/program.s", 0 };
    char *assemble[] = { assembler, "-o", "/work/program.o",
                         "/work/program.s", 0 };
    char *link[] = { compiler, "-static", "-no-pie", "/work/program.o",
                     "-o", "/work/program", 0 };
    char *run[] = { "/work/program", 0 };
    char **commands[] = { preprocess, compile, assemble, link, run };
    const char *outputs[] = { "/work/program.i", "/work/program.s",
                              "/work/program.o", "/work/program",
                              "/work/output.txt" };
    int failed = 0;
    for (int i = 0; i < 5; ++i) {
        if (failed) {
            record(results, names[i], "skipped", 0);
            continue;
        }
        const char *kind;
        int code;
        failed = launch(commands[i], i == 4 ? "/work/output.txt" : 0,
                        &kind, &code) != 0;
        if (!failed && persist(outputs[i])) {
            kind = "persist";
            code = errno;
            failed = 1;
        }
        record(results, names[i], kind, code);
    }
    if (fclose(results)) return 92;
    if (tmpfs_work && export_tmpfs()) {
        fprintf(stderr, "TOOLCHAIN tmpfs evidence copy failed errno=%d\n", errno);
        return 94;
    }
    puts("BoarOS: offline compiler probe finished");
    return 42;
}
