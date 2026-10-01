#include "../../fs/char_device_internal.h"
#include <arch/riscv/virt_rtc.h>
#include <kernel/uaccess.h>
#include <kernel/errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static uint64_t clock_ns;static int missing, fault;
enum riscv_virt_rtc_status riscv_virt_rtc_read_ns(uint64_t *p){*p=clock_ns;return missing?RISCV_VIRT_RTC_STATUS_UNAVAILABLE:RISCV_VIRT_RTC_STATUS_OK;}
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *mm,uint64_t address,const void *data,size_t n,size_t *copied){(void)mm;if(fault){*copied=0;return KERNEL_UACCESS_STATUS_FAULT;}memcpy((void *)(uintptr_t)address,data,n);*copied=n;return KERNEL_UACCESS_STATUS_OK;}
int main(void){int t[9];missing=1;assert(kernel_rtc_device.open()==-KERNEL_ENODEV);missing=0;assert(!kernel_rtc_device.open());assert(kernel_rtc_device.open()==-KERNEL_EBUSY);
 clock_ns=1709251199ULL*1000000000;assert(!kernel_rtc_device.ioctl(0,0x80247009,(uintptr_t)t));assert(t[0]==59&&t[1]==59&&t[2]==23&&t[3]==29&&t[4]==1&&t[5]==124&&t[6]==4&&t[7]==59&&t[8]==0);
 clock_ns=4107542400ULL*1000000000;assert(!kernel_rtc_device.ioctl(0,0x80247009,(uintptr_t)t));assert(t[3]==1&&t[4]==2&&t[5]==200&&t[7]==59);
 fault=1;assert(kernel_rtc_device.ioctl(0,0x80247009,(uintptr_t)t)==-KERNEL_EFAULT);assert(kernel_rtc_device.ioctl(0,0x4024700a,0)==-KERNEL_ENOTSUP);assert(kernel_rtc_device.ioctl(0,0xffff,0)==-KERNEL_ENOTTY);kernel_rtc_device.release();assert(!kernel_rtc_device.open());kernel_rtc_device.release();puts("PASS: RTC UTC leap/century, presence, exclusive claim, fault and unsupported operations");}
