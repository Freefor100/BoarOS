#include "private.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/task.h>
#include <kernel/task_state.h>
#include <kernel/uaccess.h>
#include <kernel/wait_internal.h>

#define NO_ID UINT32_MAX
#define CAP_BIT(n) (UINT64_C(1)<<(n))
#define CAP_FS_SET (CAP_BIT(KERNEL_CAP_CHOWN)|CAP_BIT(KERNEL_CAP_MKNOD)| \
    CAP_BIT(KERNEL_CAP_DAC_OVERRIDE)|CAP_BIT(KERNEL_CAP_DAC_READ_SEARCH)| \
    CAP_BIT(KERNEL_CAP_FOWNER)|CAP_BIT(KERNEL_CAP_FSETID)| \
    CAP_BIT(KERNEL_CAP_MAC_OVERRIDE)|CAP_BIT(KERNEL_CAP_LINUX_IMMUTABLE))

static void uid_fixup(struct kernel_credentials *candidate,const struct kernel_credentials *old,int filesystem)
{
    if(old->securebits&KERNEL_SECBIT_NO_SETUID_FIXUP)return;
    if(filesystem){
        if(!old->fsuid&&candidate->fsuid)candidate->effective&=~CAP_FS_SET;
        if(old->fsuid&&!candidate->fsuid)candidate->effective|=candidate->permitted&CAP_FS_SET;
        return;
    }
    if((!old->uid||!old->euid||!old->suid)&&candidate->uid&&candidate->euid&&candidate->suid){
        if(!(old->securebits&KERNEL_SECBIT_KEEP_CAPS)){candidate->permitted=0;candidate->effective=0;}
        candidate->ambient=0;
    }
    if(!old->euid&&candidate->euid)candidate->effective=0;
    if(old->euid&&!candidate->euid)candidate->effective=candidate->permitted;
}
static int is_id(uint32_t id,uint32_t a,uint32_t b,uint32_t c)
{return id==a||id==b||id==c;}

/* 各系统调用的检查先后与 references/linux/kernel/sys.c 对应。候选只在
 * 当前线程中修改，发布点同时完成 UID 账户交接与 MM 的身份变化处理。 */
static int64_t set_ids(struct kernel_task *task,unsigned operation,uint32_t a,uint32_t b,uint32_t c)
{
    const struct kernel_credentials *old=kernel_credentials_task_locked(task);
    int group=operation==143||operation==144||operation==149||operation==152;
    int filesystem=operation==151||operation==152;
    int single=operation==144||operation==146;
    int re=operation==143||operation==145;
    uint32_t r=group?old->gid:old->uid,e=group?old->egid:old->euid,
        s=group?old->sgid:old->suid,f=group?old->fsgid:old->fsuid;
    unsigned cap=group?KERNEL_CAP_SETGID:KERNEL_CAP_SETUID;
    int privileged=kernel_credentials_capable(old,cap);
    if(single&&a==NO_ID)return -KERNEL_EINVAL;
    if(filesystem&&a==NO_ID)return f;
    if(!single&&!re&&!filesystem){
        if((a==NO_ID||a==r)&&(b==NO_ID||(b==e&&b==f))&&(c==NO_ID||c==s))return 0;
        if(!privileged&&((a!=NO_ID&&!is_id(a,r,e,s))||(b!=NO_ID&&!is_id(b,r,e,s))||(c!=NO_ID&&!is_id(c,r,e,s))))
            return -KERNEL_EPERM;
    }
    struct kernel_credentials *candidate=kernel_credentials_clone(task->files.heap,old);
    if(!candidate)return filesystem?(int64_t)f:-KERNEL_ENOMEM;
    if(filesystem){
        if((!is_id(a,r,e,s)&&a!=f&&!privileged)||a==f){kernel_credentials_put(candidate);return f;}
        if(group)candidate->fsgid=a;else{candidate->fsuid=a;uid_fixup(candidate,old,1);}
        kernel_credentials_publish(task,candidate);return f;
    }
    if(single){
        if(privileged)r=s=a;
        else if(a!=r&&a!=s){kernel_credentials_put(candidate);return -KERNEL_EPERM;}
        e=f=a;
    }else if(re){
        if((a!=NO_ID&&a!=r&&a!=e&&!privileged)||(b!=NO_ID&&!is_id(b,r,e,s)&&!privileged)){
            kernel_credentials_put(candidate);return -KERNEL_EPERM;
        }
        uint32_t before_real=r;
        if(a!=NO_ID)r=a;
        if(b!=NO_ID)e=b;
        if(a!=NO_ID||(b!=NO_ID&&b!=before_real))s=e;
        f=e;
    }else{
        if(a!=NO_ID)r=a;
        if(b!=NO_ID)e=b;
        if(c!=NO_ID)s=c;
        f=e;
    }
    if(group){candidate->gid=r;candidate->egid=e;candidate->sgid=s;candidate->fsgid=f;}
    else{
        candidate->uid=r;candidate->euid=e;candidate->suid=s;candidate->fsuid=f;
        int error=kernel_credentials_set_account(candidate,r);
        if(error){kernel_credentials_put(candidate);return error;}
        uid_fixup(candidate,old,0);
    }
    kernel_credentials_publish(task,candidate);return 0;
}

static int copy_out_id(struct kernel_mm *mm,uint64_t address,uint32_t value)
{
    size_t copied=0;
    return kernel_copy_to_user(mm,address,&value,sizeof(value),&copied)==KERNEL_UACCESS_STATUS_OK&&copied==sizeof(value)?0:-KERNEL_EFAULT;
}
static int64_t groups_call(struct kernel_task *caller,struct kernel_mm *mm,
    const struct kernel_syscall_request *request)
{
    const struct kernel_credentials *old=kernel_credentials_task_locked(caller);
    int32_t size=(int32_t)(uint32_t)request->arguments[0];
    uint64_t address=request->arguments[1];
    if(request->number==158){
        if(size<0)return -KERNEL_EINVAL;
        uint32_t count=old->groups?old->groups->count:0;
        if(!size)return count;
        if((uint32_t)size<count)return -KERNEL_EINVAL;
        for(uint32_t i=0;i<count;i++)if(copy_out_id(mm,address+(uint64_t)i*4,old->groups->ids[i]))return -KERNEL_EFAULT;
        return count;
    }
    if(!kernel_credentials_capable(old,KERNEL_CAP_SETGID))return -KERNEL_EPERM;
    if((uint32_t)size>KERNEL_GROUPS_MAX)return -KERNEL_EINVAL;
    struct kernel_credential_groups *groups=kernel_credential_groups_allocate(caller->files.heap,(uint32_t)size);
    if(!groups)return -KERNEL_ENOMEM;
    for(uint32_t i=0;i<(uint32_t)size;i++){
        size_t copied=0;
        if(kernel_copy_from_user(mm,&groups->ids[i],address+(uint64_t)i*4,4,&copied)!=KERNEL_UACCESS_STATUS_OK||copied!=4){
            kernel_credential_groups_put(groups);return -KERNEL_EFAULT;
        }
        if(groups->ids[i]==NO_ID){kernel_credential_groups_put(groups);return -KERNEL_EINVAL;}
    }
    kernel_credential_groups_sort(groups);
    struct kernel_credentials *candidate=kernel_credentials_clone(caller->files.heap,old);
    if(!candidate){kernel_credential_groups_put(groups);return -KERNEL_ENOMEM;}
    kernel_credential_groups_put(candidate->groups);candidate->groups=groups;
    kernel_credentials_publish(caller,candidate);return 0;
}

enum kernel_syscall_status syscall_handle_credentials(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    const struct kernel_credentials *cred=kernel_credentials_task_locked(caller);
    unsigned op=(unsigned)request->number;
    switch(op){
    case 174:decoded->value=cred->uid;break;
    case 175:decoded->value=cred->euid;break;
    case 176:decoded->value=cred->gid;break;
    case 177:decoded->value=cred->egid;break;
    case 143:case 144:case 145:case 146:case 147:case 149:case 151:case 152:
        decoded->value=set_ids(caller,op,(uint32_t)request->arguments[0],(uint32_t)request->arguments[1],(uint32_t)request->arguments[2]);break;
    case 148:case 150:case 158:case 159:{
        struct kernel_mm *mm;
        if(kernel_task_mm_borrow_mutable(caller,&mm)!=KERNEL_TASK_STATUS_OK)return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
        if(op==158||op==159){decoded->value=groups_call(caller,mm,request);break;}
        uint32_t values[3]={op==148?cred->uid:cred->gid,op==148?cred->euid:cred->egid,op==148?cred->suid:cred->sgid};
        decoded->value=0;
        for(unsigned i=0;i<3;i++)if(copy_out_id(mm,request->arguments[i],values[i])){decoded->value=-KERNEL_EFAULT;break;}
        break;
    }
    default:return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_prctl(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    const struct kernel_credentials *old=kernel_credentials_task_locked(caller);
    uint32_t option=(uint32_t)request->arguments[0];
    uint64_t a=request->arguments[1],b=request->arguments[2],c=request->arguments[3],d=request->arguments[4];
    struct kernel_credentials *candidate=0;
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;decoded->value=-KERNEL_EINVAL;
    switch(option){
    case 3:decoded->value=kernel_credentials_dumpable_get(caller);return KERNEL_SYSCALL_STATUS_OK;
    case 4:
        if(a<=1){kernel_credentials_dumpable_set(caller,(unsigned)a);decoded->value=0;}
        return KERNEL_SYSCALL_STATUS_OK;
    case 7:decoded->value=!!(old->securebits&KERNEL_SECBIT_KEEP_CAPS);return KERNEL_SYSCALL_STATUS_OK;
    case 8:
        if(a>1)return KERNEL_SYSCALL_STATUS_OK;
        if(old->securebits&KERNEL_SECBIT_KEEP_CAPS_LOCKED){decoded->value=-KERNEL_EPERM;return KERNEL_SYSCALL_STATUS_OK;}
        break;
    case 23:
        if(a<=KERNEL_CAP_LAST_CAP)decoded->value=!!(old->bounding&(UINT64_C(1)<<a));
        return KERNEL_SYSCALL_STATUS_OK;
    case 24:
        if(!kernel_credentials_capable(old,KERNEL_CAP_SETPCAP)){decoded->value=-KERNEL_EPERM;return KERNEL_SYSCALL_STATUS_OK;}
        if(a>KERNEL_CAP_LAST_CAP)return KERNEL_SYSCALL_STATUS_OK;
        break;
    case 27:decoded->value=old->securebits;return KERNEL_SYSCALL_STATUS_OK;
    case 28:{
        const uint64_t locks=UINT64_C(0xaaa),all=UINT64_C(0xfff),unpriv=UINT64_C(0xf00);
        uint64_t changed=old->securebits^a;
        if((((old->securebits&locks)>>1)&changed)||(old->securebits&locks&~a)||(a&~all)||
            (!kernel_credentials_capable(old,KERNEL_CAP_SETPCAP)&&(!changed||(changed&~unpriv)))){
            decoded->value=-KERNEL_EPERM;return KERNEL_SYSCALL_STATUS_OK;
        }
        break;
    }
    case 38:
        if(a!=1||b||c||d)return KERNEL_SYSCALL_STATUS_OK;
        {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);caller->no_new_privs=1;}
        decoded->value=0;return KERNEL_SYSCALL_STATUS_OK;
    case 39:
        if(!(a|b|c|d))decoded->value=!!caller->no_new_privs;
        return KERNEL_SYSCALL_STATUS_OK;
    case 47:
        if(a==4){if(b|c|d)return KERNEL_SYSCALL_STATUS_OK;break;}
        if(b>KERNEL_CAP_LAST_CAP||c||d)return KERNEL_SYSCALL_STATUS_OK;
        if(a==1){decoded->value=!!(old->ambient&(UINT64_C(1)<<b));return KERNEL_SYSCALL_STATUS_OK;}
        if(a!=2&&a!=3)return KERNEL_SYSCALL_STATUS_OK;
        if(a==2&&(!(old->permitted&(UINT64_C(1)<<b))||!(old->inheritable&(UINT64_C(1)<<b))||
            (old->securebits&KERNEL_SECBIT_NO_CAP_AMBIENT_RAISE))){decoded->value=-KERNEL_EPERM;return KERNEL_SYSCALL_STATUS_OK;}
        break;
    default:return KERNEL_SYSCALL_STATUS_OK;
    }
    candidate=kernel_credentials_clone(caller->files.heap,old);
    if(!candidate){decoded->value=-KERNEL_ENOMEM;return KERNEL_SYSCALL_STATUS_OK;}
    switch(option){
    case 8:candidate->securebits=(candidate->securebits&~KERNEL_SECBIT_KEEP_CAPS)|(a?KERNEL_SECBIT_KEEP_CAPS:0);break;
    case 24:candidate->bounding&=~(UINT64_C(1)<<a);break;
    case 28:candidate->securebits=(uint32_t)a;break;
    case 47:
        if(a==4)candidate->ambient=0;
        else if(a==2)candidate->ambient|=UINT64_C(1)<<b;
        else candidate->ambient&=~(UINT64_C(1)<<b);
        break;
    }
    kernel_credentials_publish(caller,candidate);decoded->value=0;
    return KERNEL_SYSCALL_STATUS_OK;
}
