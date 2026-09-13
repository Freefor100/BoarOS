static _Thread_local int boaros_dso_tls = 700;

int boaros_tls_get(void)
{
    return boaros_dso_tls;
}

void boaros_tls_set(int value)
{
    boaros_dso_tls = value;
}

void *boaros_tls_address(void)
{
    return &boaros_dso_tls;
}
