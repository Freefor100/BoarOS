#ifndef DIFF_ABI_H
#define DIFF_ABI_H

typedef unsigned long usize;
struct abi_iovec { void *base; usize length; };
struct abi_stat {
    unsigned long dev, ino;
    unsigned int mode, nlink, uid, gid;
    unsigned long rdev, pad;
    long size;
    int blksize, pad2;
    long blocks, atime;
    unsigned long atime_nsec;
    long mtime;
    unsigned long mtime_nsec;
    long ctime;
    unsigned long ctime_nsec;
    unsigned int unused[2];
};
long abi_call(long nr, long a, long b, long c, long d, long e, long f);
#define CALL(n,a,b,c,d,e,f) abi_call(n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f))
#define SC0(n) CALL(n,0,0,0,0,0,0)
#define SC1(n,a) CALL(n,a,0,0,0,0,0)
#define SC2(n,a,b) CALL(n,a,b,0,0,0,0)
#define SC3(n,a,b,c) CALL(n,a,b,c,0,0,0)
#define SC4(n,a,b,c,d) CALL(n,a,b,c,d,0,0)
long abi_open(const char *path, long flags);
long abi_size(long fd);
long abi_offset(long fd);
void abi_record(const char *id, long ret, long size, long offset, long signal,
                const void *data, usize length);
void abi_require(int condition);
void abi_exit(long code) __attribute__((noreturn));
void abi_truncate_cases(void);
void abi_timestamp_cases(void);
void abi_readv_cases(void);
void abi_link_cases(void);
void abi_signal_wait_cases(void);
void abi_limit_cases(void);
void abi_limit_exec_probe(void) __attribute__((noreturn));
#endif
