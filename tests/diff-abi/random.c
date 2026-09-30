#include "abi.h"

void abi_random_cases(void)
{
    unsigned char buffer[256];
    abi_record("random.flags-unknown", SC3(278, buffer, 1, 8), -1,-1,0,0,0);
    abi_record("random.flags-conflict", SC3(278, buffer, 1, 6), -1,-1,0,0,0);
    abi_record("random.insecure", SC3(278, buffer, sizeof(buffer), 4), -1,-1,0,0,0);
    abi_record("random.insecure-nonblock", SC3(278, buffer, 1, 5), -1,-1,0,0,0);
    abi_record("random.secure", SC3(278, buffer, sizeof(buffer), 0), -1,-1,0,0,0);
    abi_record("random.random-flag", SC3(278, buffer, 1, 2), -1,-1,0,0,0);
    abi_record("random.zero-high-pointer", SC3(278, -1, 0, 4), -1,-1,0,0,0);
    abi_record("random.zero", SC3(278, 0, 0, 4), -1,-1,0,0,0);
    abi_record("random.fault", SC3(278, 0, 1, 4), -1,-1,0,0,0);
    long map = SC6(222, 0, 8192, 3, 0x22, -1, 0);
    abi_require(map >= 0 && SC3(226, map + 4096, 4096, 0) == 0);
    abi_record("random.partial-fault", SC3(278, map + 4092, 8, 4), -1,-1,0,0,0);
    abi_require(SC2(215, map, 8192) == 0);
    for (unsigned i=0; i<2; i++) {
        const char *path = i ? "/created-urandom" : "/created-random";
        abi_require(SC4(33, -100, path, 0020600, i ? 0x109 : 0x108) == 0);
        long fd = abi_open(path, 2);
        abi_record(i ? "random.urandom-open" : "random.random-open", fd < 0 ? fd : 0,-1,-1,0,0,0);
        if (fd < 0) continue;
        abi_record(i ? "random.urandom-read" : "random.random-read", SC3(63,fd,buffer,sizeof(buffer)),-1,-1,0,0,0);
        abi_record(i ? "random.urandom-write" : "random.random-write", SC3(64,fd,"user-data",9),-1,-1,0,0,0);
        abi_record(i ? "random.urandom-pread" : "random.random-pread", SC4(67,fd,buffer,8,17),-1,-1,0,0,0);
        abi_record(i ? "random.urandom-seek" : "random.random-seek", SC3(62,fd,99,0),-1,-1,0,0,0);
        abi_record(i ? "random.urandom-zero-high" : "random.random-zero-high", SC3(63,fd,-1,0),-1,-1,0,0,0);
        abi_record(i ? "random.urandom-seek-invalid" : "random.random-seek-invalid", SC3(62,fd,0,99),-1,-1,0,0,0);
        int bits=-1;
        long query=SC3(29,fd,0x80045200U,&bits);
        abi_record(i ? "random.urandom-entropy" : "random.random-entropy",query,-1,-1,0,&bits,sizeof(bits));
        abi_record(i ? "random.urandom-ioctl-fault" : "random.random-ioctl-fault",SC3(29,fd,0x80045200U,0),-1,-1,0,0,0);
        abi_record(i ? "random.urandom-ioctl-unknown" : "random.random-ioctl-unknown",SC3(29,fd,0xdead,0),-1,-1,0,0,0);
        long ep=SC1(20,0);
        struct __attribute__((packed)) { unsigned events; unsigned long data; } event={1,42};
        abi_require(ep>=0);
        abi_record(i ? "random.urandom-epoll" : "random.random-epoll",SC4(21,ep,1,fd,&event),-1,-1,0,0,0);
        abi_require(SC1(57,ep)==0);
        struct { int fd; short events, revents; } p = {(int)fd, 5, 0};
        struct { long sec, nsec; } timeout = {0,0};
        long ret = SC5(73,&p,1,&timeout,0,0);
        abi_record(i ? "random.urandom-poll" : "random.random-poll",ret,-1,-1,0,&p.revents,sizeof(p.revents));
        abi_require(SC1(57,fd)==0);
    }
}
