#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int server(int type, unsigned port)
{
    int fd = socket(AF_INET, type, 0);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (fd < 0 || bind(fd, (void *)&address, sizeof(address)) < 0) return -1;
    return fd;
}

static int echo_udp(int udp)
{
    struct sockaddr_in peer;
    socklen_t length = sizeof(peer);
    unsigned char buffer[1500];
    ssize_t count = recvfrom(udp, buffer, sizeof(buffer), 0, (void *)&peer, &length);
    if (count != 1472) return 1;
    for (unsigned i = 0; i < count; i++) if (buffer[i] != (unsigned char)(13 * i)) return 2;
    return sendto(udp, buffer, count, 0, (void *)&peer, length) != count;
}

static int echo_tcp(int fd)
{
    enum { TOTAL = 128 * 1024 };
    unsigned char buffer[4096];
    unsigned done = 0;
    while (done < TOTAL) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 1) return 1;
        for (unsigned i = 0; i < count; i++) {
            if (buffer[i] != (unsigned char)(17 * (i + done))) return 2;
        }
        ssize_t sent = 0;
        while (sent < count) {
            ssize_t step = write(fd, buffer + sent, count - sent);
            if (step < 1) return 3;
            sent += step;
        }
        done += count;
    }
    return read(fd, buffer, 1) != 0;
}

int main(void)
{
    int udp = server(SOCK_DGRAM, 18081), tcp = server(SOCK_STREAM, 18080);
    if (udp < 0 || tcp < 0 || listen(tcp, 1)) {
        perror("igb endpoints");
        return 1;
    }
    puts("IGB NET READY");
    fflush(stdout);
    int error = echo_udp(udp);
    if (error) {
        fprintf(stderr, "IGB UDP FAIL %d errno=%d\n", error, errno);
        return 2;
    }
    int fd = accept(tcp, NULL, NULL);
    if (fd < 0) return 3;
    error = echo_tcp(fd);
    if (error) fprintf(stderr, "IGB TCP FAIL %d errno=%d\n", error, errno);
    close(fd);
    close(tcp);
    close(udp);
    if (error) return 4;
    puts("IGB NETWORK PASS");
    return 0;
}
