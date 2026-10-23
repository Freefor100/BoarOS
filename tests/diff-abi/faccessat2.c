#include "abi.h"

#define EACCESS 0x200
#define NOFOLLOW 0x100
#define EMPTY_PATH 0x1000

static void access2(const char *id,long fd,const char *path,long mode,long flags)
{ abi_record(id,SC4(439,fd,path,mode,flags),-1,-1,0,0,0); }

void abi_faccessat2_cases(void)
{
    abi_require(!SC3(34,-100,"/access2",0700));
    long fd=SC4(56,-100,"/access2/file",0100|02,0600);
    abi_require(fd>=0);
    abi_require(!SC3(36,"missing",-100,"/access2/dangling"));
    long pathfd=abi_open("/access2/file",010000000);
    long linkfd=abi_open("/access2/dangling",010000000|0400000);
    abi_require(pathfd>=0&&linkfd>=0);
    access2("access2.file-read",-100,"/access2/file",4,0);
    access2("access2.invalid-mode-first",-9,(const char *)1,8,0x80);
    access2("access2.invalid-flags-before-pointer",-9,(const char *)1,0,0x80);
    access2("access2.pointer-before-dirfd",-9,(const char *)1,0,0);
    access2("access2.flags-int-width",-100,"/access2/file",4,1UL<<32);
    access2("access2.mode-int-width",-100,"/access2/file",1UL<<32,0);
    abi_record("access2.legacy-ignores-fourth",SC4(48,-100,"/access2/file",4,-1),-1,-1,0,0,0);
    access2("access2.empty-disabled",fd,"",0,0);
    access2("access2.empty-fd",fd,"",6,EMPTY_PATH);
    access2("access2.empty-opath",pathfd,"",4,EMPTY_PATH);
    access2("access2.empty-cwd",-100,"",0,EMPTY_PATH);
    access2("access2.empty-bad-fd",-9,"",0,EMPTY_PATH);
    access2("access2.null-is-fault",fd,0,0,EMPTY_PATH);
    access2("access2.relative-bad-fd",-9,"file",0,0);
    access2("access2.absolute-ignores-dirfd",-9,"/access2/file",0,0);
    access2("access2.follow-dangling",-100,"/access2/dangling",0,0);
    access2("access2.nofollow-dangling",-100,"/access2/dangling",0,NOFOLLOW);
    access2("access2.nofollow-link-execute",-100,"/access2/dangling",1,NOFOLLOW);
    access2("access2.empty-link-identity",linkfd,"",0,EMPTY_PATH);
    access2("access2.empty-link-nofollow",linkfd,"",0,EMPTY_PATH|NOFOLLOW);
    int pipe[2];abi_require(!SC2(59,pipe,0));
    access2("access2.empty-pipe-rw",pipe[0],"",6,EMPTY_PATH);
    access2("access2.empty-pipe-execute",pipe[0],"",1,EMPTY_PATH);
    access2("access2.pipe-not-directory",pipe[0],"child",0,EMPTY_PATH);
    abi_require(!SC1(57,pipe[0])&&!SC1(57,pipe[1]));
    long event=SC2(19,0,0);abi_require(event>=0);
    access2("access2.empty-eventfd",event,"",6,EMPTY_PATH);abi_require(!SC1(57,event));
    abi_require(!SC3(34,-100,"/access2proc",0755));
    abi_require(!SC5(40,"proc","/access2proc","proc",1,0));
    long proc=abi_open("/access2proc/self/status",0);abi_require(proc>=0);
    access2("access2.readonly-final-file",-100,"/access2proc/self/status",2,0);
    access2("access2.readonly-final-link",-100,"/access2proc/self",2,NOFOLLOW);
    access2("access2.readonly-empty-fd",proc,"",2,EMPTY_PATH);
    access2("access2.permission-before-readonly",proc,"",3,EMPTY_PATH);
    abi_require(!SC1(57,proc)&&!SC2(39,"/access2proc",0));
    abi_require(!SC3(35,-100,"/access2proc",0x200));

    /* Keep saved root so real/effective access can be compared in one process. */
    abi_require(!SC3(147,1000,0,0));
    access2("access2.real-denied",-100,"/access2/file",4,0);
    access2("access2.effective-root",-100,"/access2/file",4,EACCESS);
    access2("access2.empty-real-denied",fd,"",4,EMPTY_PATH);
    access2("access2.empty-effective-root",fd,"",4,EMPTY_PATH|EACCESS);
    abi_require(!SC3(147,0,1000,0));
    access2("access2.real-root",-100,"/access2/file",4,0);
    access2("access2.effective-denied",-100,"/access2/file",4,EACCESS);
    abi_require(!SC3(147,0,0,0));

    abi_require(!SC3(35,-100,"/access2/file",0));
    access2("access2.deleted-open-fd",fd,"",4,EMPTY_PATH);
    access2("access2.deleted-opath",pathfd,"",4,EMPTY_PATH);
    abi_require(!SC1(57,pathfd)&&!SC1(57,fd)&&!SC1(57,linkfd));
    abi_require(!SC3(35,-100,"/access2/dangling",0));
    abi_require(!SC3(35,-100,"/access2",0x200));
}
