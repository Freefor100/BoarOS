#include <kernel/scheduler.h>
#include <kernel/exec_image.h>
#include <kernel/page_cache.h>
#include <kernel/vfs.h>
void la_fault_injection_set(int);
#if ROOT_OOM_CASE == 1
enum kernel_exec_image_status __real_kernel_exec_image_prepare(const struct kernel_exec_image_request *,struct kernel_heap *,struct kernel_exec_image *,int64_t *);
enum kernel_exec_image_status __wrap_kernel_exec_image_prepare(const struct kernel_exec_image_request *request,
    struct kernel_heap *heap,struct kernel_exec_image *image,int64_t *result)
{
    la_fault_injection_set(0);
    enum kernel_exec_image_status status=__real_kernel_exec_image_prepare(request,heap,image,result);
    la_fault_injection_set(-1);return status;
}
#elif ROOT_OOM_CASE == 2
enum kernel_scheduler_status __real_kernel_user_thread_create(struct kernel_mm *,struct kernel_files *,struct kernel_fs_context *,uintptr_t,uintptr_t,uintptr_t);
enum kernel_scheduler_status __wrap_kernel_user_thread_create(struct kernel_mm *mm,struct kernel_files *files,
    struct kernel_fs_context *fs,uintptr_t entry,uintptr_t stack,uintptr_t tls)
{
    la_fault_injection_set(0);
    enum kernel_scheduler_status status=__real_kernel_user_thread_create(mm,files,fs,entry,stack,tls);
    la_fault_injection_set(-1);return status;
}
#else
int __real_kernel_page_cache_start_worker(struct kernel_page_cache *);
int __wrap_kernel_page_cache_start_worker(struct kernel_page_cache *cache)
{
    la_fault_injection_set(0);int result=__real_kernel_page_cache_start_worker(cache);
    la_fault_injection_set(-1);return result;
}
#endif
