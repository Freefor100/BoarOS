/* Exact legacy mount argument initializer on deliberately dirty storage.
 * Full mount/proc consumers are covered by the guest IPC namespace workload. */
#include "../../kernel/syscall/mount.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(void)
{
    struct mount_arguments *args=malloc(sizeof(*args));assert(args);
    for(unsigned fill=1;fill<256;fill+=17){
        memset(args,(int)fill,sizeof(*args));mount_arguments_initialize(args);
        assert(!args->parameters.ipc_namespace&&!args->parameters.pid_view);
        assert(!args->parameters.type[0]&&!args->parameters.source[0]&&!args->parameters.options[0]);
        assert((unsigned char)args->parameters.source[1]==fill);
    }
    free(args);puts("PASS legacy mount dirty arguments select caller PID/IPC view without clearing unused buffers");return 0;
}
