#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

/* Fixture EA values distinguish a filesystem read failure from a capability
 * record whose bytes were read successfully but have an invalid ABI length. */
int main(int argc,char **argv)
{
    (void)argv;
    if (argc>1) return 77;
    static const struct { const char *name,*path; int expected; } cases[]={
        {"oversized","/cap-errors/oversized",ERANGE},
        {"short-format","/cap-errors/short-format",EINVAL},
    };
    unsigned failures=0;
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        fflush(stdout);
        pid_t child=fork();
        if (child<0) {perror("fork");return 99;}
        if (!child) {
            char *const args[]={(char *)cases[i].path,"unexpected-exec",NULL};
            char *const environment[]={NULL};
            execve(cases[i].path,args,environment);
            int error=errno;
            if (getuid()!=0 || geteuid()!=0) _exit(98);
            _exit(error);
        }
        int status;
        if (waitpid(child,&status,0)!=child || !WIFEXITED(status)) return 99;
        int error=WEXITSTATUS(status);
        failures+=error!=cases[i].expected;
        printf("CRED CAP ERROR %s errno=%d expected=%d %s\n",cases[i].name,error,
            cases[i].expected,error==cases[i].expected?"PASS":"FAIL");
    }
    printf("CRED CAP ERROR END 2 failures=%u\n",failures);
    return failures!=0;
}
