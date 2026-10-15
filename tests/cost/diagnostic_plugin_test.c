#define DIAGNOSTIC_HOST_TEST
#include "diagnostic_plugin.c"
#include <assert.h>

bool qemu_plugin_read_register(struct qemu_plugin_register *handle, GByteArray *buf)
{
    (void)handle;
    uint64_t value = GUINT64_TO_LE(3ULL << 13);
    g_byte_array_append(buf, (const guint8 *)&value, 8);
    return true;
}

int main(void)
{
    expected_windows = 5;
    mstatus = (struct qemu_plugin_register *)(uintptr_t)1;
    register_bytes = g_byte_array_sized_new(8);
    struct sample start = {100, 50, 4, 2, 10}, end = {250, 150, 7, 3, 20};
    account_boundary(start);
    assert(phase == 1);
    struct block *b = new_block(0x1000, 0x2000, 3);
    assert(b && b->generated_phase == 1 && windows[0].translations == 1);
    execute(0, b);
    execute(0, b);
    entry(0, (void *)(uintptr_t)4);
    flushed(NULL);
    account_boundary(end);
    assert(phase == 2 && windows[0].wall == 150 && windows[0].cpu == 100);
    assert(windows[0].uncertainty == 30);
    assert(windows[0].voluntary == 3 && windows[0].involuntary == 1);
    assert(windows[0].proxy == 6 && b->executions[0] == 2 && b->fs[0] == 3);
    assert(windows[0].events[4] == 1 && flushes[1] == 1);
    entry(0, (void *)(uintptr_t)4);
    assert(windows[0].events[4] == 1);  /* Outside window must stay excluded. */
    block_count = MAX_TBS;
    assert(!new_block(1, 2, 3) && invalid);
    invalid = 0;
    phase = expected_windows * 2;
    account_boundary(end);
    assert(invalid && phase == 10);  /* Extra boundary cannot become a valid trace. */
    g_byte_array_free(register_bytes, TRUE);
    puts("diagnostic collector: accounting/boundaries/capacity passed");
    return 0;
}
