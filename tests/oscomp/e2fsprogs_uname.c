/* Native regression fixture: give the real mke2fs a controlled UTS identity. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

int __wrap_uname(struct utsname *value)
{
    const char *release = getenv("OSCOMP_TEST_UTS_RELEASE");
    const char *version = getenv("OSCOMP_TEST_UTS_VERSION");
    if (!release || !version) {
        errno = EINVAL;
        return -1;
    }
    memset(value, 0, sizeof(*value));
    snprintf(value->sysname, sizeof(value->sysname), "Linux");
    snprintf(value->release, sizeof(value->release), "%s", release);
    snprintf(value->version, sizeof(value->version), "%s", version);
    snprintf(value->machine, sizeof(value->machine), "x86_64");
    return 0;
}
