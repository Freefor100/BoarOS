#ifndef BOAROS_TEST_SHARED_MAPPING_H
#define BOAROS_TEST_SHARED_MAPPING_H

static int check_shared_anonymous_mapping(void)
{
    unsigned char *pages = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int child_to_parent[2];
    int parent_to_child[2];
    char signal;
    int status;
    pid_t child;

    if (pages == MAP_FAILED) return 1;
    pages[0] = 0x11;
    unsigned char *independent = mmap(0, 4096, PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_ANONYMOUS, 12345, 0);
    if (independent == MAP_FAILED || independent[0] != 0U ||
        munmap(independent, 4096) != 0) return 8;
    errno = 0;
    if (mmap(pages, 4096, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
             -1, 0) != MAP_FAILED || errno != EEXIST ||
        pages[0] != 0x11) return 9;
    errno = 0;
    if (syscall(SYS_futex, pages, 1, 1, 0, 0, 0) != -1 ||
        errno != ENOTSUP) return 7;
    if (pipe(child_to_parent) != 0 || pipe(parent_to_child) != 0)
        return 2;
    child = fork();
    if (child < 0) return 3;
    if (child == 0) {
        close(child_to_parent[0]);
        close(parent_to_child[1]);
        if (pages[0] != 0x11) _exit(11);
        pages[0] = 0x22;
        pages[2 * 4096] = 0x44;
        if (write(child_to_parent[1], "r", 1) != 1 ||
            read(parent_to_child[0], &signal, 1) != 1 ||
            pages[4096] != 0x33) _exit(12);
        _exit(0);
    }
    close(child_to_parent[1]);
    close(parent_to_child[0]);
    if (read(child_to_parent[0], &signal, 1) != 1 ||
        pages[0] != 0x22 || pages[2 * 4096] != 0x44) return 4;
    pages[4096] = 0x33;
    if (write(parent_to_child[1], "a", 1) != 1 ||
        waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 5;
    if (close(child_to_parent[0]) != 0 ||
        close(parent_to_child[1]) != 0 ||
        munmap(pages, 3 * 4096) != 0) return 6;
    return 0;
}

#endif
