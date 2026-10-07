#include <stdint.h>
int main(void)
{
    *(volatile uint64_t *)(uintptr_t)8=0;
    return 90;
}
