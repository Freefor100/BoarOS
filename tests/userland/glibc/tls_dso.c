__thread int library_tls = 31;

int library_bump(void)
{
    return ++library_tls;
}
