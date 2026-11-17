/* Host Linux: lookup semantics and rejection of an unavailable huge size.
 * BoarOS: additionally require fail-closed until SHM_HUGETLB is implemented. */
#define _GNU_SOURCE
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#ifndef SHM_HUGETLB
#define SHM_HUGETLB 04000
#endif
#define CHECK(x) do { if (!(x)) { dprintf(2,"SHM_HUGE_FLAGS FAIL line=%d errno=%d %s\n",__LINE__,errno,#x); exit(1); } } while (0)
#define ERROR(x,e) do { errno=0;CHECK((x)==-1 && errno==(e)); } while (0)
int main(void)
{
    key_t key=(key_t)(0x47000000U|((unsigned)getpid()&0xffffffU));
    int id=shmget(key,1,IPC_CREAT|IPC_EXCL|0600);CHECK(id>=0);
    int found=shmget(key,0,SHM_HUGETLB|0600);
    int exclusive=shmget(key,0,SHM_HUGETLB|IPC_CREAT|IPC_EXCL|0600),exerr=errno;
    int large=shmget(key,2,SHM_HUGETLB|0600),largerr=errno;
    CHECK(!shmctl(id,IPC_RMID,0));
    CHECK(found==id&&exclusive==-1&&exerr==EEXIST&&large==-1&&largerr==EINVAL);
    ERROR(shmget(key,1,SHM_HUGETLB|0600),ENOENT);
#ifdef BOAROS_EXPECT_NO_SHM_HUGETLB
    ERROR(shmget(IPC_PRIVATE,1,SHM_HUGETLB|0600),ENOSYS);
    ERROR(shmget(key,1,SHM_HUGETLB|IPC_CREAT|0600),ENOSYS);
#else
    /* log2(size)=1 cannot name a huge hstate. No pool mutation or privilege needed. */
    ERROR(shmget(IPC_PRIVATE,1,SHM_HUGETLB|(1U<<26)|0600),EINVAL);
#endif
    puts("SHM_HUGE_FLAGS PASS");
    return 0;
}
