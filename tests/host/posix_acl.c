#include <kernel/posix_acl.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct acl5 {uint32_t version;struct kernel_acl_entry e[5];};
int kernel_credentials_in_group(const struct kernel_credentials *c,uint32_t gid)
{return c->fsgid==gid;}
int kernel_credentials_capable(const struct kernel_credentials *c,unsigned capability)
{return !!(c->effective & (UINT64_C(1) << capability));}
int main(void)
{
    struct acl5 a={2,{{1,6,UINT32_MAX},{2,0,12345},{4,4,UINT32_MAX},{16,4,UINT32_MAX},{32,0,UINT32_MAX}}};
    struct kernel_credentials c={.fsuid=12345,.fsgid=123};
    assert(!kernel_acl_validate(&a,sizeof(a)));
    assert(kernel_acl_permission(&a,sizeof(a),0,123,&c,4)==-KERNEL_EACCES);
    assert(kernel_acl_dac_permission(&a,sizeof(a),0100640,0,123,&c,4)==-KERNEL_EACCES);
    c.effective=UINT64_C(1)<<KERNEL_CAP_DAC_OVERRIDE;
    assert(!kernel_acl_dac_permission(&a,sizeof(a),0100640,0,123,&c,4));
    assert(kernel_acl_dac_permission(&a,sizeof(a),0100640,0,123,&c,1)==-KERNEL_EACCES);
    c.effective=0;
    c.fsuid=222;assert(!kernel_acl_permission(&a,sizeof(a),0,123,&c,4));
    unsigned char disk[128],output[128];size_t size=0,written=0;
    assert(!kernel_acl_to_ext4(&a,sizeof(a),disk,sizeof(disk),&size));assert(size==28);
    assert(!kernel_acl_from_ext4(disk,size,output,sizeof(output),&written));
    assert(written==sizeof(a)&&!memcmp(&a,output,sizeof(a)));
    assert(kernel_acl_from_ext4(disk,size-1,output,sizeof(output),&written)==-KERNEL_EINVAL);
    assert(kernel_acl_from_ext4(disk,4,output,sizeof(output),&written)==-KERNEL_ENODATA);
    assert(kernel_acl_to_ext4(&a,sizeof(a),disk,8,&written)==-KERNEL_ERANGE && written==size);
    struct acl5 invalid=a;
    invalid.e[1].id=UINT32_MAX;
    assert(kernel_acl_validate(&invalid,sizeof(invalid))==-KERNEL_EINVAL);
    invalid=a;invalid.e[3].tag=32;
    assert(kernel_acl_validate(&invalid,sizeof(invalid))==-KERNEL_EINVAL);
    uint32_t mode=0666;a.e[1].perm=6;a.e[3].perm=6;
    assert(kernel_acl_create(&a,sizeof(a),&mode)==1 && mode==0660);
    assert(!kernel_acl_chmod(&a,sizeof(a),0640) && a.e[3].perm==4);
    a.e[1].perm=9;assert(kernel_acl_validate(&a,sizeof(a))==-KERNEL_EINVAL);
    puts("posix acl: validation, permission, disk encoding, inheritance and chmod passed");return 0;
}
