#define _GNU_SOURCE
#include <stdlib.h>
__attribute__((constructor)) static void mark_loaded(void)
{if(setenv("CRED_PRELOADED","1",1))abort();}
