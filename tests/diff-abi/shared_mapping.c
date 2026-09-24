#include "abi.h"

void abi_shared_mapping_cases(void)
{
    volatile unsigned char *pages = (void *)CALL(222, 0, 3 * 4096, 3,
                                                 0x21, -1, 0);
    int child_to_parent[2], parent_to_child[2], status = 0;
    char signal;
    long child;
    unsigned char values[3];

    abi_require((long)pages >= 0);
    pages[0] = 0x11U;
    abi_require(SC2(59, child_to_parent, 0) == 0);
    abi_require(SC2(59, parent_to_child, 0) == 0);
    child = CALL(220, 17, 0, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (child == 0) {
        SC1(57, child_to_parent[0]);
        SC1(57, parent_to_child[1]);
        if (pages[0] != 0x11U) abi_exit(11);
        pages[0] = 0x22U;
        pages[8192] = 0x44U;
        if (SC3(64, child_to_parent[1], "r", 1) != 1 ||
            SC3(63, parent_to_child[0], &signal, 1) != 1 ||
            pages[4096] != 0x33U) abi_exit(12);
        abi_exit(0);
    }
    SC1(57, child_to_parent[1]);
    SC1(57, parent_to_child[0]);
    abi_require(SC3(63, child_to_parent[0], &signal, 1) == 1);
    values[0] = pages[0];
    values[2] = pages[8192];
    pages[4096] = 0x33U;
    values[1] = pages[4096];
    abi_require(SC3(64, parent_to_child[1], "a", 1) == 1);
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("shared-anon.fork", 0, -1, -1, status, values,
               sizeof(values));
    abi_require(SC1(57, child_to_parent[0]) == 0);
    abi_require(SC1(57, parent_to_child[1]) == 0);
    abi_require(SC2(215, pages, 3 * 4096) == 0);
}
