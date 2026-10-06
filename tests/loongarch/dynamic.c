/* Share the static portable catalogue; add the existing real dlopen TLS case. */
#define LA_PTHREAD_ENTRY la_static_pthread_main
#include "pthread.c"
int main(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[1],"execed"))return execed_mode(argv[2]);
    if(check_dlopen_tls()) {fputs("LA dynamic DSO TLS failed\n",stderr);return 1;}
    puts("LA dynamic DSO TLS passed");
    return la_static_pthread_main(argc,argv);
}
