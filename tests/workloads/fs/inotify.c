#include "../../diff-abi/abi.h"
#include "../../common/raw_syscall.h"

long abi_call(long nr,long a,long b,long c,long d,long e,long f)
{ return test_syscall6(nr,a,b,c,d,e,f); }

enum { ACCESS=1, MODIFY=2, ATTRIB=4, CLOSE_WRITE=8, CLOSE_NOWRITE=16,
       OPEN=32, FROM=64, TO=128, CREATE=256, DELETE=512, DELETE_SELF=1024,
       UNMOUNT=0x2000, OVERFLOW=0x4000, IGNORED=0x8000, ONESHOT=0x80000000U };
struct event { int wd; unsigned mask,cookie,len; };
struct epoll_event { unsigned events; unsigned long data; };
static unsigned char buf[4096] __attribute__((aligned(16)));
static unsigned used,pos;
static void stop(long status)
{
    SC1(93,status);
    for (;;) {}
}
static void fail(unsigned line)
{
    char out[]="INOTIFY FAIL 0000\n";
    for (int i=16;i>=13;i--) {out[i]='0'+line%10;line/=10;}
    SC3(64,1,out,sizeof(out)-1);
    stop(1);
}
#define CHECK(c) do {if (!(c)) fail(__LINE__);} while (0)
static int same(const char *a,const char *b)
{
    while (*a && *a==*b) {a++;b++;}
    return *a==*b;
}
static struct event *next(long fd,int wd,unsigned mask,const char *name)
{
    if (pos==used) {
        long n=SC3(63,fd,buf,sizeof(buf));
        CHECK(n>=16);used=n;pos=0;
    }
    CHECK(used-pos>=sizeof(struct event));
    struct event *e=(void *)(buf+pos);
    CHECK(e->wd==wd && e->mask==mask && e->len<=used-pos-sizeof(*e));
    CHECK(name ? e->len && same((char *)(e+1),name) : e->len==0);
    pos+=sizeof(*e)+e->len;
    return e;
}
void abi_main(void *sp)
{
    (void)sp;
    CHECK(SC1(26,1)==-22);
    long fd=SC1(26,04000|02000000);
    CHECK(fd>=0 && SC3(25,fd,1,0)==1);
    CHECK(SC3(25,fd,3,0)==04000);
    CHECK(SC3(63,fd,buf,sizeof(buf))==-11);
    CHECK(SC3(27,fd,"/missing",CREATE)==-2);
    CHECK(SC3(27,fd,"/data",CREATE|0x01000000)==-20);
    CHECK(SC3(34,-100,"/watched",0700)==0);
    int wd=SC3(27,fd,"/watched",CREATE|DELETE|FROM|TO|OPEN|MODIFY|ACCESS|CLOSE_WRITE);
    CHECK(wd>0);
    CHECK(SC3(27,fd,"/watched",CREATE|0x10000000)==-17);
    CHECK(SC3(27,fd,"/watched",ATTRIB|0x20000000)==wd);
    long ep=SC1(20,0);
    struct epoll_event event={1,0x12345678}, ready;
    CHECK(ep>=0 && SC4(21,ep,1,fd,&event)==0);
    long file=SC4(56,-100,"/watched/old",0102,0600);
    CHECK(file>=0 && SC3(64,file,"x",1)==1 && SC1(57,file)==0);
    CHECK(SC6(22,ep,&ready,1,0,0,0)==1 && ready.events==1 && ready.data==event.data);
    int queued;
    CHECK(SC3(29,fd,0x541b,&queued)==0 && queued==128);
    CHECK(SC3(63,fd,buf,31)==-22);
    struct abi_iovec iov[2]={{buf,8},{buf+8,120}};
    CHECK(SC3(65,fd,iov,2)==-22);
    next(fd,wd,CREATE,"old");next(fd,wd,OPEN,"old");
    next(fd,wd,MODIFY,"old");next(fd,wd,CLOSE_WRITE,"old");
    CHECK(used==pos);
    CHECK(SC6(22,ep,&ready,1,0,0,0)==0);
    CHECK(SC3(53,-100,"/watched/old",0640)==0);
    next(fd,wd,ATTRIB,"old");CHECK(used==pos);
    file=SC4(56,-100,"/watched/old",0,0);
    char content;
    CHECK(file>=0 && SC3(63,file,&content,1)==1 && content=='x' && SC1(57,file)==0);
    next(fd,wd,OPEN,"old");next(fd,wd,ACCESS,"old");CHECK(used==pos);
    int fwd=SC3(27,fd,"/watched/old",DELETE_SELF|ATTRIB);
    CHECK(fwd>0 && fwd!=wd);
    CHECK(SC4(38,-100,"/watched/old",-100,"/watched/new")==0);
    unsigned cookie=next(fd,wd,FROM,"old")->cookie;
    CHECK(cookie && next(fd,wd,TO,"new")->cookie==cookie && used==pos);
    CHECK(SC3(35,-100,"/watched/new",0)==0);
    next(fd,fwd,ATTRIB,0);next(fd,wd,DELETE,"new");
    next(fd,fwd,DELETE_SELF,0);next(fd,fwd,IGNORED,0);
    CHECK(used==pos && SC2(28,fd,fwd)==-22);
    long duplicate=SC1(23,fd);
    CHECK(duplicate>=0 && SC1(57,ep)==0 && SC1(57,fd)==0);
    fd=duplicate;
    CHECK(SC2(28,fd,wd)==0);
    next(fd,wd,IGNORED,0);CHECK(used==pos);
    CHECK(SC3(34,-100,"/watched/mnt",0700)==0);
    CHECK(SC5(40,"tmpfs","/watched/mnt","tmpfs",0,0)==0);
    int mounted=SC3(27,fd,"/watched/mnt",CREATE|ONESHOT);
    CHECK(mounted>0);
    file=SC4(56,-100,"/watched/mnt/a",0102,0600);
    CHECK(file>=0 && SC1(57,file)==0);
    next(fd,mounted,CREATE,"a");next(fd,mounted,IGNORED,0);CHECK(used==pos);
    mounted=SC3(27,fd,"/watched/mnt",MODIFY|ATTRIB);
    CHECK(mounted>0);
    file=SC4(56,-100,"/watched/mnt/a",2,0);
    CHECK(file>=0);
    for (unsigned i=0;i<2100;i++) {
        CHECK(SC2(52,file,(i&1)?0640:0600)==0);
        CHECK(SC4(68,file,"z",1,0)==1);
    }
    CHECK(SC1(57,file)==0);
    unsigned overflow=0;
    long count;
    while ((count=SC3(63,fd,buf,sizeof(buf)))>0) {
        for (unsigned at=0;at<(unsigned)count;) {
            struct event *record=(void *)(buf+at);
            CHECK((unsigned)count-at>=sizeof(*record) &&
                  record->len<=(unsigned)count-at-sizeof(*record));
            if (record->mask==OVERFLOW) {CHECK(record->wd==-1 && !record->len);overflow++;}
            else CHECK(record->wd==mounted && (record->mask==MODIFY || record->mask==ATTRIB));
            at+=sizeof(*record)+record->len;
        }
    }
    CHECK(count==-11 && overflow==1 && SC2(39,"/watched/mnt",0)==0);
    next(fd,mounted,UNMOUNT,0);next(fd,mounted,IGNORED,0);CHECK(used==pos);
    CHECK(SC1(57,fd)==0);
    static const char pass[]="INOTIFY PASS\n";
    SC3(64,1,pass,sizeof(pass)-1);
    stop(0);
}
