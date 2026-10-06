#define _GNU_SOURCE
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Arrival skew at the two public FIFO start barriers. Socket traffic and
 * measurements still execute the real workload; no private fd numbers matter. */
struct arrivals { atomic_uint clients, second_entered, first_waiting; };
static struct arrivals *arrivals;
static unsigned client, fifo_reads;
extern int __real_connect(int, const struct sockaddr *, socklen_t);
extern ssize_t __real_read(int, void *, size_t);
__attribute__((constructor)) static void setup(void)
{
    arrivals = mmap(0, sizeof(*arrivals), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(arrivals != MAP_FAILED);
    atomic_store(&arrivals->first_waiting, 1);
}
int __wrap_connect(int fd, const struct sockaddr *address, socklen_t size)
{
    int result = __real_connect(fd, address, size);
    if (!result && address->sa_family == AF_INET)
        client = atomic_fetch_add(&arrivals->clients, 1) + 1;
    return result;
}
ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
    struct stat state;
    if (!client || fstat(fd, &state) || !S_ISFIFO(state.st_mode))
        return __real_read(fd, buffer, size);
    unsigned phase = ++fifo_reads;
    if (client == 5 && phase == 1) {
        while (!atomic_load(&arrivals->second_entered)) usleep(1000);
        /* Let a fast client attempt phase two while this first wait is pending. */
        usleep(20000);
        atomic_store(&arrivals->first_waiting, 0);
    }
    if (phase == 2) atomic_store(&arrivals->second_entered, 1);
    ssize_t result = __real_read(fd, buffer, size);
    if (phase == 2 && result > 0 && atomic_load(&arrivals->first_waiting)) {
        fputs("BARRIER phase two consumed a phase-one token\n", stderr);
        _exit(77);
    }
    return result;
}
