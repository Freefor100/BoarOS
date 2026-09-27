#include <stdio.h>

#define SEED 0x27u

static unsigned fold(const unsigned *values, unsigned count)
{
    unsigned result = SEED;
    for (unsigned index = 0; index < count; ++index)
        result = (result * 33u) ^ values[index];
    return result;
}

int main(void)
{
    const unsigned values[] = { 3u, 5u, 8u, 13u, 21u };
    unsigned result = fold(values, sizeof(values) / sizeof(values[0]));
    if (printf("BoarOS offline C result=%u\n", result) < 0) return 2;
    return result == 1522623313u ? 0 : 3;
}
