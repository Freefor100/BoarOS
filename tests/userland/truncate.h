/* Resident mappings must observe truncate even without a subsequent fault.
 * Run early, before the signal tests install handlers. */
static int check_resident_truncate(void)
{
    const size_t page = 4096;
    unsigned char contents[4096];
    memset(contents, 'F', sizeof(contents));
    for (int cow = 0; cow < 2; cow++) {
        for (int protected = 0; protected < 2; protected++) {
            int fd = open("/truncate-map", O_CREAT | O_TRUNC | O_RDWR, 0600);
            if (fd < 0 || write(fd, contents, page) != (ssize_t)page ||
                write(fd, contents, page) != (ssize_t)page) return 1;
            volatile unsigned char *p = mmap(0, 2 * page,
                PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED || p[page] != 'F') return 2;
            if (cow) p[page] = 'C';
            if (protected && mprotect((void *)(p + page), page, PROT_NONE))
                return 3;
            /* The child owns a different, already resident MM. */
            int gate[2];
            if (pipe(gate)) return 4;
            pid_t child = fork();
            if (child < 0) return 5;
            if (child == 0) {
                char token;
                if (read(gate[0], &token, 1) != 1) _exit(90);
                if (protected && mprotect((void *)(p + page), page,
                                           PROT_READ | PROT_WRITE)) _exit(91);
                volatile unsigned char value = p[page];
                (void)value;
                _exit(92);
            }
            if (unlink("/truncate-map") || ftruncate(fd, page) || close(fd) ||
                write(gate[1], "x", 1) != 1) return 6;
            int status;
            if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) ||
                WTERMSIG(status) != SIGBUS) return 7;
            close(gate[0]);
            close(gate[1]);
            child = fork();
            if (child == 0) {
                if (protected && mprotect((void *)(p + page), page,
                                           PROT_READ | PROT_WRITE)) _exit(91);
                volatile unsigned char value = p[page];
                (void)value;
                _exit(92);
            }
            if (child < 0 || waitpid(child, &status, 0) != child ||
                !WIFSIGNALED(status) || WTERMSIG(status) != SIGBUS) return 8;
            if (p[0] != 'F' || munmap((void *)p, 2 * page)) return 9;
        }
    }
    /* Same file through independent OFDs, offset mappings, and private tails.
     * A prior file write evicts the cache index while PTEs retain old pages. */
    for (int evict = 0; evict < 2; evict++) {
        int fd = open("/truncate-tail", O_CREAT | O_TRUNC | O_RDWR, 0600);
        if (fd < 0 || write(fd, contents, page) != (ssize_t)page ||
            write(fd, contents, page) != (ssize_t)page) return 10;
        int other = open("/truncate-tail", O_RDWR);
        if (other < 0) return 11;
        volatile unsigned char *clean = mmap(0, page, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE, fd, page);
        volatile unsigned char *dirty = mmap(0, page, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE, other, page);
        if (clean == MAP_FAILED || dirty == MAP_FAILED || clean[0] != 'F' ||
            clean[100] != 'F') return 12;
        dirty[0] = 'D';
        dirty[100] = 'P';
        if (evict && (lseek(fd, 0, SEEK_SET) != 0 || write(fd, "X", 1) != 1))
            return 13;
        if (ftruncate(other, page + 32) || clean[0] != 'F' || clean[31] != 'F' ||
            clean[32] != 0 || clean[100] != 0 || dirty[0] != 'D' ||
            dirty[100] != 'P') return 14;
        /* COW created before fork stays private in the child's tail too. */
        pid_t child = fork();
        if (child == 0) _exit(dirty[100] == 'P' && clean[100] == 0 ? 0 : 1);
        int status;
        if (child < 0 || waitpid(child, &status, 0) != child || status != 0)
            return 15;
        if (ftruncate(other, 2 * page) || clean[100] != 0 ||
            munmap((void *)clean, page) || munmap((void *)dirty, page) ||
            close(fd) || close(other) || unlink("/truncate-tail")) return 16;
    }
    return 0;
}
