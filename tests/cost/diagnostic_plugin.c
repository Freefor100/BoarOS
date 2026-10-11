/* Read-only, single-vCPU observer. All I/O is deferred until QEMU exits. */
#define _GNU_SOURCE
#include <qemu-plugin.h>
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
#define WINDOWS 16
#define EVENTS 7
#define MAX_TBS 131072
struct sample { uint64_t wall, cpu, voluntary, involuntary, wall_bracket; };
struct block {
    uint64_t pc, haddr, insns, executions[WINDOWS];
    unsigned fs[WINDOWS], generated_phase;
};
struct window {
    struct sample start;
    uint64_t wall, cpu, uncertainty, voluntary, involuntary, translations, generated, proxy, events[EVENTS];
};
/* Stable callback addresses survive global code-cache flush; a full buffer invalidates the run. */
static struct block blocks[MAX_TBS];
static struct window windows[WINDOWS];
static unsigned phase, block_count, invalid, expected_windows, cpu_count;
static uint64_t clock_pc, event_pc[EVENTS];
static const char *output_path;
static struct qemu_plugin_register *mstatus;
static GByteArray *register_bytes;
static unsigned flushes[2 * WINDOWS + 1];

static struct sample sample(void)
{
    struct timespec wall, cpu, after;
    struct rusage usage;
    if (clock_gettime(CLOCK_MONOTONIC, &wall) ||
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) || getrusage(RUSAGE_THREAD, &usage) ||
        clock_gettime(CLOCK_MONOTONIC, &after)) {
        invalid = 1;
        return (struct sample){0};
    }
    uint64_t before_ns = wall.tv_sec * 1000000000ULL + wall.tv_nsec;
    uint64_t after_ns = after.tv_sec * 1000000000ULL + after.tv_nsec;
    if (after_ns < before_ns) { invalid = 1; return (struct sample){0}; }
    return (struct sample){before_ns,
        cpu.tv_sec * 1000000000ULL + cpu.tv_nsec, usage.ru_nvcsw, usage.ru_nivcsw,
        after_ns - before_ns};
}

static void account_boundary(struct sample value)
{
    if (phase >= 2 * expected_windows) { invalid = 1; return; }
    struct window *w = &windows[phase / 2];
    if (phase & 1) {
        if (value.wall < w->start.wall || value.cpu < w->start.cpu ||
            value.voluntary < w->start.voluntary || value.involuntary < w->start.involuntary) {
            invalid = 1;
            return;
        }
        w->wall = value.wall - w->start.wall;
        w->cpu = value.cpu - w->start.cpu;
        w->uncertainty = value.wall_bracket + w->start.wall_bracket;
        w->voluntary = value.voluntary - w->start.voluntary;
        w->involuntary = value.involuntary - w->start.involuntary;
    } else w->start = value;
    phase++;
}

static void boundary(unsigned cpu, void *data)
{
    (void)data;
    if (cpu) { invalid = 1; return; }
    account_boundary(sample());
}

static void entry(unsigned cpu, void *data)
{
    if (cpu) { invalid = 1; return; }
    if ((phase & 1) && phase < 2 * expected_windows)
        windows[phase / 2].events[(uintptr_t)data]++;
}

static void flushed(void *data)
{
    (void)data;
    if (phase > 2 * WINDOWS) { invalid = 1; return; }
    flushes[phase]++;  /* QEMU plugin flush is global code cache, not guest TLB invalidation. */
}

static struct block *new_block(uint64_t pc, uint64_t haddr, uint64_t insns)
{
    if (block_count == MAX_TBS) { invalid = 1; return NULL; }
    struct block *b = &blocks[block_count++];
    b->pc = pc; b->haddr = haddr; b->insns = insns; b->generated_phase = phase;
    for (unsigned i = 0; i < WINDOWS; i++) b->fs[i] = 4;
    if ((phase & 1) && phase < 2 * expected_windows) {
        windows[phase / 2].translations++;
        windows[phase / 2].generated += insns;
    }
    return b;
}

static void execute(unsigned cpu, void *data)
{
    if (cpu) { invalid = 1; return; }
    if (!(phase & 1) || phase >= 2 * expected_windows) return;
    struct block *b = data;
    unsigned index = phase / 2;
    if (!b->executions[index]) {
        if (!mstatus || !register_bytes) { invalid = 1; return; }
        g_byte_array_set_size(register_bytes, 0);
        if (!qemu_plugin_read_register(mstatus, register_bytes) || register_bytes->len != 8) {
            invalid = 1;
            return;
        }
        uint64_t value;
        memcpy(&value, register_bytes->data, 8);
        b->fs[index] = (GUINT64_FROM_LE(value) >> 13) & 3;
    }
    b->executions[index]++;
    windows[index].proxy += b->insns;  /* A trap may leave TB early: this is not retired instructions. */
}

static void cpu_init(unsigned cpu, void *data)
{
    (void)data;
    cpu_count++;
    if (cpu || cpu_count != 1) { invalid = 1; return; }
    GArray *registers = qemu_plugin_get_registers();
    for (unsigned i = 0; i < registers->len; i++) {
        qemu_plugin_reg_descriptor *r = &g_array_index(registers, qemu_plugin_reg_descriptor, i);
        if (!strcmp(r->name, "mstatus")) mstatus = r->handle;
    }
    g_array_free(registers, TRUE);
    register_bytes = g_byte_array_sized_new(8); /* Reserve once; no hot-callback allocation. */
    if (!mstatus) invalid = 1;
}

static void translated(struct qemu_plugin_tb *tb, void *data)
{
    (void)data;
    uint64_t count = qemu_plugin_tb_n_insns(tb);
    if (!count) { invalid = 1; return; }
    struct block *b = new_block(qemu_plugin_tb_vaddr(tb),
        (uintptr_t)qemu_plugin_insn_haddr(qemu_plugin_tb_get_insn(tb, 0)), count);
    if (!b) return;
    for (size_t i = 0; i < count; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t pc = qemu_plugin_insn_vaddr(insn);
        if (pc == clock_pc)
            qemu_plugin_register_vcpu_insn_exec_cb(insn, boundary, QEMU_PLUGIN_CB_NO_REGS, NULL);
        for (unsigned e = 0; e < EVENTS; e++) if (pc == event_pc[e])
            qemu_plugin_register_vcpu_insn_exec_cb(insn, entry, QEMU_PLUGIN_CB_NO_REGS, (void *)(uintptr_t)e);
    }
    qemu_plugin_register_vcpu_tb_exec_cb(tb, execute, QEMU_PLUGIN_CB_R_REGS, b);
}

static void finish(void *data)
{
    (void)data;
    FILE *out = fopen(output_path, "wx");
    if (!out) return; /* Missing trace is rejected by the controller. */
    fprintf(out, "TRACE 1 %u %u %u %u %u\n", phase, invalid, cpu_count, !!mstatus, block_count);
    for (unsigned i = 0; i <= 2 * expected_windows; i++) fprintf(out, "FLUSH %u %u\n", i, flushes[i]);
    for (unsigned i = 0; i < expected_windows; i++) {
        struct window *w = &windows[i];
        fprintf(out, "WINDOW %u %llu %llu %llu %llu %llu %llu %llu %llu", i,
            (unsigned long long)w->wall, (unsigned long long)w->cpu,
            (unsigned long long)w->uncertainty,
            (unsigned long long)w->voluntary, (unsigned long long)w->involuntary,
            (unsigned long long)w->translations, (unsigned long long)w->generated,
            (unsigned long long)w->proxy);
        for (unsigned e = 0; e < EVENTS; e++) fprintf(out, " %llu", (unsigned long long)w->events[e]);
        fputc('\n', out);
    }
    for (unsigned i = 0; i < block_count; i++) {
        struct block *b = &blocks[i];
        fprintf(out, "TB %u %llx %llx %llu %u", i, (unsigned long long)b->pc,
            (unsigned long long)b->haddr, (unsigned long long)b->insns, b->generated_phase);
        for (unsigned j = 0; j < expected_windows; j++)
            fprintf(out, " %llu %u", (unsigned long long)b->executions[j], b->fs[j]);
        fputc('\n', out);
    }
    fprintf(out, "END %u\n", block_count);
    fclose(out);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv)
{
    if (!info->system_emulation || info->system.smp_vcpus != 1 || argc != 4 + EVENTS ||
        !g_str_has_prefix(argv[0], "clock=") || !g_str_has_prefix(argv[1], "output=") ||
        !g_str_has_prefix(argv[2], "windows=") || !g_str_has_prefix(argv[3], "schema=1")) return -1;
    clock_pc = g_ascii_strtoull(argv[0] + 6, NULL, 0);
    output_path = g_strdup(argv[1] + 7);
    expected_windows = g_ascii_strtoull(argv[2] + 8, NULL, 10);
    if (!clock_pc || output_path[0] != '/' || !expected_windows || expected_windows > WINDOWS) return -1;
    for (unsigned i = 0; i < EVENTS; i++) {
        if (!g_str_has_prefix(argv[4 + i], "event=")) return -1;
        event_pc[i] = g_ascii_strtoull(argv[4 + i] + 6, NULL, 0);
        if (!event_pc[i]) return -1;
    }
    qemu_plugin_register_vcpu_init_cb(id, cpu_init, NULL);
    qemu_plugin_register_flush_cb(id, flushed, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, translated, NULL);
    qemu_plugin_register_atexit_cb(id, finish, NULL);
    return 0;
}
