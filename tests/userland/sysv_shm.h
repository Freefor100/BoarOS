#ifndef USERLAND_SYSV_SHM_H
#define USERLAND_SYSV_SHM_H

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

static int check_sysv_shm(void)
{
    /* 1. IPC_PRIVATE creation */
    int shmid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0666);
    if (shmid < 0) return 1;

    /* 2. Attach */
    void *ptr = shmat(shmid, NULL, 0);
    if (ptr == (void *)-1) return 2;

    /* Write data */
    strcpy((char *)ptr, "Hello SysV SHM");

    /* 3. IPC_STAT */
    struct shmid_ds ds;
    if (shmctl(shmid, IPC_STAT, &ds) != 0) return 3;
    if (ds.shm_nattch != 1 || ds.shm_segsz != 4096) return 4;

    /* 4. Child process fork and verify shared memory across processes */
    pid_t pid = fork();
    if (pid < 0) return 5;
    if (pid == 0) {
        /* In child */
        if (strcmp((char *)ptr, "Hello SysV SHM") != 0) _exit(10);
        strcpy((char *)ptr, "Child was here");
        if (shmdt(ptr) != 0) _exit(11);
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return 6;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 7;

    /* Verify child's write is visible to parent */
    if (strcmp((char *)ptr, "Child was here") != 0) return 8;

    /* 5. IPC_RMID */
    if (shmctl(shmid, IPC_RMID, NULL) != 0) return 9;

    /* Segment still attached: reading must still work */
    if (strcmp((char *)ptr, "Child was here") != 0) return 10;

    /* Detach parent: nattch reaches 0, segment is destroyed */
    if (shmdt(ptr) != 0) return 11;

    /* Try to attach destroyed segment: should fail with EINVAL */
    if (shmat(shmid, NULL, 0) != (void *)-1 || errno != EINVAL) return 12;

    /* 6. Named key tests */
    key_t key = (key_t)0x8765;
    int named_id = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0666);
    if (named_id < 0) return 13;

    /* Duplicate creation with IPC_EXCL should fail with EEXIST */
    if (shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0666) != -1 || errno != EEXIST) return 14;

    /* Lookup existing key should succeed */
    int lookup_id = shmget(key, 4096, 0);
    if (lookup_id != named_id) return 15;

    /* Clean up named segment */
    if (shmctl(named_id, IPC_RMID, NULL) != 0) return 16;

    /* Lookup after removal should fail with ENOENT */
    if (shmget(key, 4096, 0) != -1 || errno != ENOENT) return 17;

    return 0;
}

#endif
