#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

/* Execute the unmodified upstream binary, preserving its output and wait
 * status. Hex encoding prevents applet output from forging result markers. */
int main(void)
{
    static char *const commands[][6] = {
        {"/busybox", "true", 0},
        {"/busybox", "false", 0},
        {"/busybox", "echo", "inventory", 0},
        {"/busybox", "cat", "/inventory-data", 0},
        {"/busybox", "ls", "/inventory-dir", 0},
        {"/busybox", "sh", "-c", "printf 'shell\\n'; /busybox cat /inventory-data", 0},
    };
    static const char *const names[] = {"true", "false", "echo", "cat", "ls", "shell"};
    char *const environment[] = {"PATH=/", "LC_ALL=C", 0};

    setvbuf(stdout, 0, _IONBF, 0);
    puts("INVENTORY BEGIN 1");
    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        int descriptors[2], status;
        pid_t child;
        unsigned char bytes[128];
        ssize_t count;
        if (pipe(descriptors) != 0 || (child = fork()) < 0) {
            printf("INVENTORY DRIVER_ERROR %s %d\n", names[n], errno);
            return 1;
        }
        if (child == 0) {
            close(descriptors[0]);
            if (dup2(descriptors[1], 1) < 0 || dup2(descriptors[1], 2) < 0) {
                _exit(126);
            }
            close(descriptors[1]);
            execve("/busybox", commands[n], environment);
            dprintf(2, "execve errno=%d\n", errno);
            _exit(127);
        }
        close(descriptors[1]);
        printf("INVENTORY OUTPUT %s ", names[n]);
        while ((count = read(descriptors[0], bytes, sizeof(bytes))) > 0) {
            for (ssize_t i = 0; i < count; i++) printf("%02x", bytes[i]);
        }
        puts(".");
        close(descriptors[0]);
        if (count < 0 || waitpid(child, &status, 0) != child) {
            printf("INVENTORY DRIVER_ERROR %s %d\n", names[n], errno);
            return 2;
        }
        printf("INVENTORY STATUS %s %d\n", names[n], status);
    }
    puts("INVENTORY END 6");
    reboot(RB_POWER_OFF);
    return 42;
}
