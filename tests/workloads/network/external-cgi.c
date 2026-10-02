#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "external-cgi:%d: %s errno=%d\n", __LINE__, #x, errno); return 1; } } while (0)

int main(int argc, char **argv)
{
    const char *length = getenv("CONTENT_LENGTH"); CHECK(length != NULL && *length != 0);
    char *end; errno = 0; unsigned long remaining = strtoul(length, &end, 10);
    CHECK(errno == 0 && *end == 0 && remaining == 16UL * 1024UL * 1024UL);
    CHECK(argc == 1 || argc == 2);
    int output = open(argc == 2 ? argv[1] : "/upload.bin", O_CREAT | O_TRUNC | O_WRONLY, 0600);
    CHECK(output >= 0);
    unsigned char buffer[8192];
    while (remaining > 0) {
        size_t wanted = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        ssize_t size = read(STDIN_FILENO, buffer, wanted);
        if (size < 0 && errno == EINTR) continue;
        CHECK(size > 0 && (size_t)size <= wanted);
        for (size_t done = 0; done < (size_t)size;) {
            ssize_t written = write(output, buffer + done, (size_t)size - done);
            if (written < 0 && errno == EINTR) continue;
            CHECK(written > 0 && (size_t)written <= (size_t)size - done); done += (size_t)written;
        }
        remaining -= (unsigned long)size;
    }
    CHECK(close(output) == 0);
    CHECK(fputs("Content-Type: text/plain\r\nContent-Length: 10\r\n\r\nUPLOAD OK\n", stdout) >= 0);
    CHECK(fflush(stdout) == 0);
    return 0;
}
