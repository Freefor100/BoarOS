#include "abi.h"
#define RECORD(name, value) abi_record(name,value,-1,-1,0,0,0)
void abi_environment_cases(void)
{
 char buffer[4];
 RECORD("log.close",SC3(116,0,0,0));RECORD("log.open",SC3(116,1,0,0));
 RECORD("log.capacity",SC3(116,10,0,0)>0);
 RECORD("log.unread",SC3(116,9,0,0)>=0);
 RECORD("log.invalid",SC3(116,11,0,0));
 RECORD("log.null",SC3(116,3,0,100));RECORD("log.negative",SC3(116,3,buffer,-1));
 RECORD("log.zero",SC3(116,3,buffer,0));RECORD("log.fault",SC3(116,3,1,1048576));
 RECORD("log.level0",SC3(116,8,0,0));RECORD("log.level9",SC3(116,8,0,9));
 RECORD("log.level1",SC3(116,8,0,1));RECORD("log.off",SC3(116,6,0,0));
 RECORD("log.on",SC3(116,7,0,0));RECORD("log.level8",SC3(116,8,0,8));
 RECORD("log.clear_zero",SC3(116,4,buffer,0));RECORD("log.clear",SC3(116,5,0,0));
 RECORD("log.after_clear",SC3(116,3,buffer,sizeof(buffer)));
 long fd=abi_open("/dev/rtc0",0);abi_require(fd>=0);
 RECORD("rtc.open",0);long other=abi_open("/dev/rtc0",0);RECORD("rtc.exclusive",other);
 int t[9];long ret=SC3(29,fd,0x80247009UL,t);RECORD("rtc.read",ret);abi_require(ret==0);
 RECORD("rtc.calendar",t[0]>=0&&t[0]<60&&t[1]>=0&&t[1]<60&&t[2]>=0&&t[2]<24&&t[3]>0&&t[3]<32&&t[4]>=0&&t[4]<12&&t[5]>=120&&t[6]>=0&&t[6]<7&&t[7]>=0&&t[7]<366&&t[8]==0);
 RECORD("rtc.signed_command",SC3(29,fd,(long)(int)0x80247009U,t));
 RECORD("rtc.fault",SC3(29,fd,0x80247009UL,1));
 RECORD("rtc.unknown",SC3(29,fd,0xf00d,0));
 long copy=SC1(23,fd);abi_require(copy>=0&&SC1(57,fd)==0);
 RECORD("rtc.dup_exclusive",abi_open("/dev/rtc0",0));abi_require(SC1(57,copy)==0);
 fd=abi_open("/dev/rtc0",0);RECORD("rtc.reopen",fd>=0?0:fd);abi_require(fd>=0&&SC1(57,fd)==0);
}
