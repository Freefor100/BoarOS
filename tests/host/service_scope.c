#include <kernel/scheduler.h>
#include <assert.h>
static unsigned queries;
#if BOAROS_USER_SMP
static unsigned begins,ends;
void kernel_service_scope_begin(struct kernel_service_scope *scope) { scope->entered=1;begins++; }
void kernel_service_scope_end(struct kernel_service_scope *scope) { ends+=scope->entered; }
#endif
static int requires_service(void) { queries++;return 1; }
int main(void)
{
    { KERNEL_SERVICE_SCOPE_IF(scope, requires_service()); }
#if BOAROS_USER_SMP
    assert(queries==1 && begins==1 && ends==1);
#else
    assert(!queries);
#endif
    return 0;
}
