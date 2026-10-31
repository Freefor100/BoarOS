#include <kernel/posix_acl.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <string.h>

enum { USER_OBJ=1, USER=2, GROUP_OBJ=4, GROUP=8, MASK=16, OTHER=32 };
static uint32_t word(const void *p){uint32_t n;memcpy(&n,p,4);return n;}
static uint16_t half(const void *p){uint16_t n;memcpy(&n,p,2);return n;}
static void putword(void *p,uint32_t n){memcpy(p,&n,4);}
static void puthalf(void *p,uint16_t n){memcpy(p,&n,2);}
static struct kernel_acl_entry entry(const void *value,size_t i)
{struct kernel_acl_entry e;memcpy(&e,(const char *)value+4+i*8,8);return e;}
static void putentry(void *value,size_t i,struct kernel_acl_entry e)
{memcpy((char *)value+4+i*8,&e,8);}
int kernel_acl_validate(const void *value,size_t size)
{
    if(!value || size<4 || (size-4)%8 || word(value)!=2)return -KERNEL_EINVAL;
    unsigned state=USER_OBJ,needs_mask=0;
    for(size_t i=0;i<(size-4)/8;i++) {
        struct kernel_acl_entry e=entry(value,i);if(e.perm&~7U)return -KERNEL_EINVAL;
        switch(e.tag) {
        case USER_OBJ:if(state!=USER_OBJ)return -KERNEL_EINVAL;state=USER;break;
        case USER:if(state!=USER || e.id==UINT32_MAX)return -KERNEL_EINVAL;needs_mask=1;break;
        case GROUP_OBJ:if(state!=USER)return -KERNEL_EINVAL;state=GROUP;break;
        case GROUP:if(state!=GROUP || e.id==UINT32_MAX)return -KERNEL_EINVAL;needs_mask=1;break;
        case MASK:if(state!=GROUP)return -KERNEL_EINVAL;state=OTHER;break;
        case OTHER:if(state!=OTHER && !(state==GROUP&&!needs_mask))return -KERNEL_EINVAL;state=0;break;
        default:return -KERNEL_EINVAL;
        }
    }
    return state ? -KERNEL_EINVAL:0;
}
int kernel_acl_normalize(void *value,size_t size)
{
    if (kernel_acl_validate(value,size)) return -KERNEL_EINVAL;
    for (size_t i=0;i<(size-4)/8;i++) {
        struct kernel_acl_entry e=entry(value,i);
        if (e.tag!=USER && e.tag!=GROUP) e.id=UINT32_MAX;
        putentry(value,i,e);
    }
    return 0;
}
int kernel_acl_permission(const void *value,size_t size,uint32_t uid,uint32_t gid,const struct kernel_credentials *c,unsigned access)
{
    if(kernel_acl_validate(value,size))return -KERNEL_EIO;
    size_t n=(size-4)/8;unsigned mask=7,group_seen=0,group_allowed=0;
    for(size_t i=0;i<n;i++){struct kernel_acl_entry e=entry(value,i);if(e.tag==MASK)mask=e.perm;}
    for(size_t i=0;i<n;i++) {
        struct kernel_acl_entry e=entry(value,i);
        if(e.tag==USER_OBJ && c->fsuid==uid)return (access & ~e.perm)?-KERNEL_EACCES:0;
        if(e.tag==USER && c->fsuid==e.id)return (access & ~(e.perm&mask))?-KERNEL_EACCES:0;
        if((e.tag==GROUP_OBJ && kernel_credentials_in_group(c,gid)) ||
           (e.tag==GROUP && kernel_credentials_in_group(c,e.id))) {
            group_seen=1;if(!(access & ~(e.perm&mask)))group_allowed=1;
        }
        if(e.tag==OTHER)return group_seen?(group_allowed?0:-KERNEL_EACCES):
            ((access & ~e.perm)?-KERNEL_EACCES:0);
    }
    return -KERNEL_EIO;
}
int kernel_acl_dac_permission(const void *value,size_t size,uint32_t mode,uint32_t uid,uint32_t gid,
    const struct kernel_credentials *cred,unsigned access)
{
    int result=kernel_acl_permission(value,size,uid,gid,cred,access);
    if (result!=-KERNEL_EACCES) return result;
    int directory=(mode & 0170000U)==0040000U;
    if (kernel_credentials_capable(cred,KERNEL_CAP_DAC_OVERRIDE) &&
        (directory || !(access & 1U) || (mode & 0111U))) return 0;
    if (kernel_credentials_capable(cred,KERNEL_CAP_DAC_READ_SEARCH) &&
        (directory ? !(access & 2U) : access==4U)) return 0;
    return result;
}
int kernel_acl_mode(const void *value,size_t size,uint32_t *mode)
{
    if(!mode || kernel_acl_validate(value,size))return -KERNEL_EINVAL;
    unsigned owner=0,group=0,other=0,extended=0,mask_present=0,mask=0;
    for(size_t i=0;i<(size-4)/8;i++) {struct kernel_acl_entry e=entry(value,i);
        if(e.tag==USER_OBJ)owner=e.perm;else if(e.tag==GROUP_OBJ)group=e.perm;
        else if(e.tag==OTHER)other=e.perm;else if(e.tag==MASK){mask=e.perm;mask_present=extended=1;}
        else extended=1;
    }
    *mode=(*mode & ~0777U)|(owner<<6)|((mask_present?mask:group)<<3)|other;
    return (int)extended;
}
int kernel_acl_create(void *value,size_t size,uint32_t *mode)
{
    if(!mode || kernel_acl_validate(value,size))return -KERNEL_EINVAL;
    unsigned has_mask=0;for(size_t i=0;i<(size-4)/8;i++)if(entry(value,i).tag==MASK)has_mask=1;
    for(size_t i=0;i<(size-4)/8;i++) {struct kernel_acl_entry e=entry(value,i);
        if(e.tag==USER_OBJ)e.perm &= (*mode>>6)&7U;
        else if(e.tag==OTHER)e.perm &= *mode&7U;
        else if(e.tag==MASK || (e.tag==GROUP_OBJ&&!has_mask))e.perm &= (*mode>>3)&7U;
        putentry(value,i,e);
    }
    return kernel_acl_mode(value,size,mode);
}
int kernel_acl_chmod(void *value,size_t size,uint32_t mode)
{
    if(kernel_acl_validate(value,size))return -KERNEL_EINVAL;
    unsigned has_mask=0;for(size_t i=0;i<(size-4)/8;i++)if(entry(value,i).tag==MASK)has_mask=1;
    for(size_t i=0;i<(size-4)/8;i++){struct kernel_acl_entry e=entry(value,i);
        if(e.tag==USER_OBJ)e.perm=(mode>>6)&7U;else if(e.tag==OTHER)e.perm=mode&7U;
        else if(e.tag==MASK || (e.tag==GROUP_OBJ&&!has_mask))e.perm=(mode>>3)&7U;
        putentry(value,i,e);
    }return 0;
}
int kernel_acl_to_ext4(const void *value,size_t size,void *out,size_t cap,size_t *written)
{
    if(!written || kernel_acl_validate(value,size))return -KERNEL_EINVAL;
    size_t needed=4;for(size_t i=0;i<(size-4)/8;i++){unsigned t=entry(value,i).tag;needed+=(t==USER||t==GROUP)?8:4;}
    *written=needed;if(!out)return 0;if(cap<needed)return -KERNEL_ERANGE;
    putword(out,1);size_t p=4;
    for(size_t i=0;i<(size-4)/8;i++){struct kernel_acl_entry e=entry(value,i);puthalf((char *)out+p,e.tag);puthalf((char *)out+p+2,e.perm);p+=4;
        if(e.tag==USER || e.tag==GROUP){putword((char *)out+p,e.id);p+=4;}}
    return 0;
}
int kernel_acl_from_ext4(const void *value,size_t size,void *out,size_t cap,size_t *written)
{
    if(!value || !written || size<4 || word(value)!=1)return -KERNEL_EINVAL;
    size_t p=4,count=0;
    while(p<size){if(size-p<4)return -KERNEL_EINVAL;unsigned tag=half((const char *)value+p);p+=4;
        if(tag==USER || tag==GROUP){if(size-p<4)return -KERNEL_EINVAL;p+=4;}
        else if(tag!=USER_OBJ && tag!=GROUP_OBJ && tag!=MASK && tag!=OTHER)return -KERNEL_EINVAL;
        count++;}
    if (!count) return -KERNEL_ENODATA;
    size_t needed=4+count*8;*written=needed;if(!out)return 0;if(cap<needed)return -KERNEL_ERANGE;
    putword(out,2);p=4;
    for(size_t i=0;i<count;i++){struct kernel_acl_entry e={half((const char *)value+p),half((const char *)value+p+2),UINT32_MAX};p+=4;
        if(e.tag==USER || e.tag==GROUP){e.id=word((const char *)value+p);p+=4;}putentry(out,i,e);}
    return kernel_acl_validate(out,needed)?-KERNEL_EINVAL:0;
}
