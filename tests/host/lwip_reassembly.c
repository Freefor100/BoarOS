#include "lwip/ip4_frag.h"
#include "lwip/inet_chksum.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fixed upstream fixture deliberately keeps its unpatched API. */
#ifdef BOAROS_TEST_UPSTREAM_REASS
extern void ip4_reass_cleanup_netif(struct netif *) __attribute__((weak));
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

struct wire_buffer {
    struct pbuf_custom pc;
    unsigned *frees;
    _Alignas(8) unsigned char bytes[];
};

static struct netif interface_a;
static struct netif interface_b;
static const unsigned char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWX01234567";
static unsigned live_wire_buffers;

static void wire_free(struct pbuf *p)
{
    struct wire_buffer *wire = (struct wire_buffer *)p;
    if (wire->frees != NULL) {
        ++*wire->frees;
    }
    --live_wire_buffers;
    free(wire);
}

static struct pbuf *wire_allocate(unsigned length, unsigned *frees)
{
    struct wire_buffer *wire = malloc(sizeof(*wire) + length);
    struct pbuf *p;
    if (wire == NULL) {
        abort();
    }
    wire->frees = frees;
    wire->pc.custom_free_function = wire_free;
    p = pbuf_alloced_custom(PBUF_RAW, (u16_t)length, PBUF_REF, &wire->pc,
                           wire->bytes, (u16_t)length);
    if (p == NULL) {
        abort();
    }
    ++live_wire_buffers;
    return p;
}

/* Literal IPv4 addresses/ID and a literal 32-byte payload keep semantic
 * expectations independent of lwIP's fragment parser. Offset is in bytes. */
static struct pbuf *fragment(unsigned offset, unsigned length, int more,
                             unsigned proto, unsigned id, unsigned *frees)
{
    const unsigned char header[20] = {
        0x45, 0, 0, 0, 0x42, 0x01, 0, 0, 64, 17, 0, 0,
        192, 0, 2, 1, 192, 0, 2, 2
    };
    struct pbuf *p = wire_allocate(20 + length, frees);
    unsigned char *bytes = p->payload;
    unsigned field = offset / 8 + (more ? 0x2000 : 0);
    memcpy(bytes, header, sizeof(header));
    bytes[2] = (unsigned char)((20 + length) >> 8);
    bytes[3] = (unsigned char)(20 + length);
    bytes[4] = (unsigned char)(id >> 8);
    bytes[5] = (unsigned char)id;
    bytes[6] = (unsigned char)(field >> 8);
    bytes[7] = (unsigned char)field;
    bytes[9] = (unsigned char)proto;
    for (unsigned i = 0; i < length; ++i) {
        bytes[20 + i] = alphabet[(offset + i) % 32];
    }
    ((struct ip_hdr *)bytes)->_chksum = inet_chksum(bytes, 20);
    return p;
}

static struct pbuf *input(struct pbuf *p, struct netif *netif)
{
#ifdef BOAROS_TEST_UPSTREAM_REASS
    ip_data.current_input_netif = netif;
    return ip4_reass(p);
#else
    return ip4_reass(p, netif);
#endif
}

static int complete_literal(struct pbuf *p, unsigned proto)
{
    unsigned char bytes[52];
    CHECK(p != NULL);
    CHECK(p->tot_len == 52);
    CHECK(pbuf_copy_partial(p, bytes, 52, 0) == 52);
    CHECK(bytes[2] == 0 && bytes[3] == 52);
    CHECK(bytes[6] == 0 && bytes[7] == 0 && bytes[9] == proto);
    CHECK(memcmp(bytes + 20, alphabet, 32) == 0);
    CHECK(inet_chksum(bytes, 20) == 0);
    return 0;
}

static void expire_all(void)
{
    for (unsigned i = 0; i <= IP_REASS_MAXAGE; ++i) {
        ip_reass_tmr();
    }
}

static int normal(void)
{
    unsigned frees = 0;
    struct pbuf *p;
    CHECK(input(fragment(0, 16, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(input(fragment(16, 8, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    p = input(fragment(24, 8, 0, 17, 0x4201, &frees), &interface_a);
    CHECK(complete_literal(p, 17) == 0);
    CHECK(frees == 0);
    pbuf_free(p);
    CHECK(frees == 3);
    return 0;
}

static int reverse_duplicate(void)
{
    unsigned frees = 0;
    struct pbuf *p;
    CHECK(input(fragment(24, 8, 0, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(input(fragment(24, 8, 0, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(frees == 1);
    CHECK(input(fragment(16, 8, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    p = input(fragment(0, 16, 1, 17, 0x4201, &frees), &interface_a);
    CHECK(complete_literal(p, 17) == 0);
    pbuf_free(p);
    CHECK(frees == 4);
    return 0;
}

static int covered_range_duplicate(void)
{
    unsigned frees = 0;
    struct pbuf *p;
    CHECK(input(fragment(0, 16, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(input(fragment(8, 8, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(frees == 1);
    p = input(fragment(16, 16, 0, 17, 0x4201, &frees), &interface_a);
    CHECK(complete_literal(p, 17) == 0);
    pbuf_free(p);
    CHECK(frees == 3);
    return 0;
}

static int contiguous_range_duplicate(void)
{
    unsigned frees = 0;
    struct pbuf *p;
    CHECK(input(fragment(0, 16, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(input(fragment(16, 8, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(input(fragment(8, 16, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(frees == 1);
    p = input(fragment(24, 8, 0, 17, 0x4201, &frees), &interface_a);
    CHECK(complete_literal(p, 17) == 0);
    pbuf_free(p);
    CHECK(frees == 4);
    return 0;
}

static int duplicate_declares_final(void)
{
    unsigned frees = 0;
    CHECK(input(fragment(16, 8, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(input(fragment(16, 8, 0, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(frees == 1);
    CHECK(input(fragment(24, 8, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(frees == 3);
    CHECK(lwip_stats.memp[MEMP_REASSDATA]->used == 0);
    return 0;
}

static int different_protocol(void)
{
    unsigned frees = 0;
    struct pbuf *p;
    CHECK(input(fragment(0, 16, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(input(fragment(16, 16, 0, 6, 0x4201, &frees), &interface_a) == NULL);
    CHECK(frees == 0);
    p = input(fragment(16, 16, 0, 17, 0x4201, &frees), &interface_a);
    CHECK(complete_literal(p, 17) == 0);
    pbuf_free(p);
    p = input(fragment(0, 16, 1, 6, 0x4201, &frees), &interface_a);
    CHECK(complete_literal(p, 6) == 0);
    pbuf_free(p);
    CHECK(frees == 4);
    return 0;
}

static int different_interface(void)
{
    unsigned frees = 0;
    struct pbuf *p;
    CHECK(input(fragment(0, 16, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(input(fragment(16, 16, 0, 17, 0x4201, &frees), &interface_b) == NULL);
    p = input(fragment(16, 16, 0, 17, 0x4201, &frees), &interface_a);
    CHECK(complete_literal(p, 17) == 0);
    pbuf_free(p);
    p = input(fragment(0, 16, 1, 17, 0x4201, &frees), &interface_b);
    CHECK(complete_literal(p, 17) == 0);
    pbuf_free(p);
    CHECK(frees == 4);
    return 0;
}

static int partial_overlap(void)
{
    unsigned frees = 0;
    CHECK(input(fragment(0, 16, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(input(fragment(8, 16, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(frees == 2);
    CHECK(lwip_stats.memp[MEMP_REASSDATA]->used == 0);
    CHECK(input(fragment(16, 16, 0, 17, 0x4201, &frees), &interface_a) == NULL);
    expire_all();
    CHECK(frees == 3);
    return 0;
}

static int overlap_same_start(void)
{
    unsigned frees = 0;
    CHECK(input(fragment(0, 16, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(input(fragment(0, 24, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(frees == 2);
    CHECK(lwip_stats.memp[MEMP_REASSDATA]->used == 0);
    return 0;
}

static int conflicting_final(void)
{
    unsigned frees = 0;
    CHECK(input(fragment(24, 8, 0, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(input(fragment(16, 8, 0, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(frees == 2);
    CHECK(lwip_stats.memp[MEMP_REASSDATA]->used == 0);
    CHECK(input(fragment(24, 8, 0, 17, 0x4202, &frees), &interface_a) == NULL);
    CHECK(input(fragment(32, 8, 0, 17, 0x4202, &frees), &interface_a) == NULL);
    CHECK(frees == 4);
    CHECK(lwip_stats.memp[MEMP_REASSDATA]->used == 0);
    /* A final fragment wholly covered by an earlier non-final range is
     * still corrupt if it would shrink the known extent. */
    CHECK(input(fragment(16, 16, 1, 17, 0x4203, &frees), &interface_a) == NULL);
    CHECK(input(fragment(16, 8, 0, 17, 0x4203, &frees), &interface_a) == NULL);
    CHECK(frees == 6);
    CHECK(lwip_stats.memp[MEMP_REASSDATA]->used == 0);
    return 0;
}

static int fragment_beyond_final(void)
{
    unsigned frees = 0;
    CHECK(input(fragment(24, 8, 0, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(input(fragment(32, 8, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(frees == 2);
    CHECK(lwip_stats.memp[MEMP_REASSDATA]->used == 0);
    CHECK(input(fragment(32, 8, 1, 17, 0x4202, &frees), &interface_a) == NULL);
    CHECK(input(fragment(24, 8, 0, 17, 0x4202, &frees), &interface_a) == NULL);
    CHECK(frees == 4);
    CHECK(lwip_stats.memp[MEMP_REASSDATA]->used == 0);
    return 0;
}

static int missing_fragment_timeout(void)
{
    unsigned frees = 0;
    CHECK(input(fragment(0, 16, 1, 17, 0x4201, &frees), &interface_a) == NULL);
    CHECK(input(fragment(24, 8, 0, 17, 0x4201, &frees), &interface_a) == NULL);
    for (unsigned i = 0; i < IP_REASS_MAXAGE; ++i) {
        ip_reass_tmr();
    }
    CHECK(frees == 0);
    ip_reass_tmr();
    CHECK(frees == 2);
    CHECK(lwip_stats.memp[MEMP_REASSDATA]->used == 0);
    return 0;
}

static int pbuf_pressure(void)
{
    unsigned old_frees = 0, incoming_frees = 0, new_frees = 0;
    for (unsigned i = 0; i < IP_REASS_MAX_PBUFS; ++i) {
        CHECK(input(fragment(i * 8, 8, 1, 17, 0x4201, &old_frees),
                    &interface_a) == NULL);
    }
    CHECK(old_frees == 0);
    CHECK(input(fragment(IP_REASS_MAX_PBUFS * 8, 8, 1, 17, 0x4201,
                         &incoming_frees), &interface_a) == NULL);
    CHECK(incoming_frees == 1 && old_frees == 0);
    CHECK(input(fragment(0, 8, 1, 17, 0x4202, &new_frees), &interface_a) == NULL);
    CHECK(old_frees == IP_REASS_MAX_PBUFS && new_frees == 0);
    expire_all();
    CHECK(new_frees == 1);
    return 0;
}

static int object_pressure(void)
{
    unsigned frees[MEMP_NUM_REASSDATA + 1] = {0};
    for (unsigned i = 0; i < MEMP_NUM_REASSDATA; ++i) {
        CHECK(input(fragment(0, 8, 1, 17, 0x4201 + i, &frees[i]),
                    &interface_a) == NULL);
        ip_reass_tmr();
    }
    CHECK(input(fragment(0, 8, 1, 17, 0x4301, &frees[MEMP_NUM_REASSDATA]),
                &interface_a) == NULL);
    CHECK(frees[0] == 1);
    for (unsigned i = 1; i <= MEMP_NUM_REASSDATA; ++i) {
        CHECK(frees[i] == 0);
    }
    CHECK(lwip_stats.memp[MEMP_REASSDATA]->used == MEMP_NUM_REASSDATA);
    expire_all();
    for (unsigned i = 0; i <= MEMP_NUM_REASSDATA; ++i) {
        CHECK(frees[i] == 1);
    }
    return 0;
}

static int duplicate_at_capacity(void)
{
    unsigned frees_a = 0, frees_b = 0, duplicate_frees = 0;
    for (unsigned i = 0; i < IP_REASS_MAX_PBUFS - 1; ++i) {
        CHECK(input(fragment(i * 8, 8, 1, 17, 0x4201, &frees_a),
                    &interface_a) == NULL);
    }
    CHECK(input(fragment(0, 8, 1, 17, 0x4202, &frees_b), &interface_a) == NULL);
    CHECK(input(fragment(0, 8, 1, 17, 0x4201, &duplicate_frees),
                &interface_a) == NULL);
    CHECK(duplicate_frees == 1 && frees_a == 0 && frees_b == 0);
    expire_all();
    CHECK(frees_a == IP_REASS_MAX_PBUFS - 1 && frees_b == 1);
    return 0;
}

static int overlap_at_capacity(void)
{
    unsigned frees = 0;
    for (unsigned i = 0; i < IP_REASS_MAX_PBUFS; ++i) {
        CHECK(input(fragment(i * 8, 8, 1, 17, 0x4201, &frees),
                    &interface_a) == NULL);
    }
    CHECK(input(fragment((IP_REASS_MAX_PBUFS - 1) * 8, 16, 1, 17, 0x4201,
                         &frees), &interface_a) == NULL);
    CHECK(frees == IP_REASS_MAX_PBUFS + 1);
    CHECK(lwip_stats.memp[MEMP_REASSDATA]->used == 0);
    return 0;
}

static int cleanup_interface(void)
{
    unsigned frees_a = 0, frees_b = 0;
    struct pbuf *p;
#ifdef BOAROS_TEST_UPSTREAM_REASS
    CHECK(ip4_reass_cleanup_netif != NULL);
#endif
    CHECK(input(fragment(24, 8, 0, 17, 0x4201, &frees_a), &interface_a) == NULL);
    CHECK(input(fragment(0, 16, 1, 17, 0x4201, &frees_b), &interface_b) == NULL);
    CHECK(input(fragment(0, 8, 1, 6, 0x4202, &frees_a), &interface_a) == NULL);
    CHECK(input(fragment(0, 16, 1, 17, 0x4201, &frees_a), &interface_a) == NULL);
    ip4_reass_cleanup_netif(&interface_a);
    CHECK(frees_a == 3 && frees_b == 0);
    CHECK(lwip_stats.memp[MEMP_REASSDATA]->used == 1);
    ip4_reass_cleanup_netif(&interface_a);
    CHECK(frees_a == 3);
    p = input(fragment(16, 16, 0, 17, 0x4201, &frees_b), &interface_b);
    CHECK(complete_literal(p, 17) == 0);
    pbuf_free(p);
    CHECK(frees_b == 2);
    return 0;
}

static int cleanup_external_reference(void)
{
    unsigned frees = 0;
    struct pbuf *held = fragment(0, 16, 1, 17, 0x4201, &frees);
#ifdef BOAROS_TEST_UPSTREAM_REASS
    CHECK(ip4_reass_cleanup_netif != NULL);
#endif
    pbuf_ref(held);
    CHECK(input(held, &interface_a) == NULL);
    ip4_reass_cleanup_netif(&interface_a);
    CHECK(frees == 0);
    CHECK(lwip_stats.memp[MEMP_REASSDATA]->used == 0);
    pbuf_free(held);
    CHECK(frees == 1);
    ip4_reass_cleanup_netif(&interface_a);
    CHECK(frees == 1);
    return 0;
}

struct capture {
    struct pbuf *packets[45];
    unsigned count;
    unsigned frees;
    int hold_tx;
};

static err_t capture_wire(struct netif *netif, struct pbuf *p,
                           const ip4_addr_t *destination)
{
    struct capture *capture = netif->state;
    struct pbuf *wire;
    (void)destination;
    if (capture->count >= 45) {
        abort();
    }
    if (capture->hold_tx) {
        /* A NIC may keep a real header/custom-ref TX chain until completion. */
        pbuf_ref(p);
        wire = p;
    } else {
        /* A wire frame has one RX owner even if TX used multiple pbufs. */
        wire = wire_allocate(p->tot_len, &capture->frees);
        if (pbuf_copy_partial(p, wire->payload, p->tot_len, 0) != p->tot_len) {
            abort();
        }
    }
    capture->packets[capture->count++] = wire;
    return ERR_OK;
}

static int maximum_datagram(int reverse)
{
    struct capture capture = {0};
    struct netif transmit = {0};
    unsigned source_frees = 0;
    struct pbuf *source = wire_allocate(65535, &source_frees);
    struct pbuf *result = NULL;
    unsigned char *bytes = source->payload;
    unsigned char copy[1024];
    const unsigned char header[28] = {
        0x45, 0, 0xff, 0xff, 0x42, 0x03, 0, 0, 64, 17, 0, 0,
        192, 0, 2, 1, 192, 0, 2, 2,
        0x31, 0x32, 0x33, 0x34, 0xff, 0xeb, 0, 0
    };
    memcpy(bytes, header, sizeof(header));
    for (unsigned i = 28; i < 65535; ++i) {
        bytes[i] = (unsigned char)(i % 251);
    }
    transmit.mtu = 1500;
    transmit.output = capture_wire;
    transmit.state = &capture;
    CHECK(ip4_frag(source, &transmit, NULL) == ERR_OK);
    CHECK(capture.count == 45);
    CHECK(capture.packets[0]->tot_len == 1500);
    CHECK(capture.packets[44]->tot_len == 415);
    CHECK(lwip_stats.memp[MEMP_FRAG_PBUF]->used == 0);
    for (unsigned i = 0; i < 45; ++i) {
        unsigned index = reverse ? 44 - i : i;
        result = input(capture.packets[index], &interface_a);
        if (i < 44) {
            CHECK(result == NULL && capture.frees == 0);
        }
    }
    CHECK(result != NULL && result->tot_len == 65535);
    for (unsigned offset = 20; offset < 65535; offset += sizeof(copy)) {
        unsigned len = 65535 - offset;
        if (len > sizeof(copy)) {
            len = sizeof(copy);
        }
        CHECK(pbuf_copy_partial(result, copy, (u16_t)len, (u16_t)offset) == len);
        CHECK(memcmp(copy, bytes + offset, len) == 0);
    }
    pbuf_free(result);
    pbuf_free(source);
    CHECK(capture.frees == 45 && source_frees == 1);
    return 0;
}

static int maximum_forward(void) { return maximum_datagram(0); }
static int maximum_reverse(void) { return maximum_datagram(1); }

static int fragmenter_reference_cleanup(void)
{
    struct capture capture = { .hold_tx = 1 };
    struct netif transmit = {0};
    unsigned frees = 0;
    struct pbuf *source = fragment(0, 32, 0, 17, 0x4201, &frees);
    transmit.mtu = 36;
    transmit.output = capture_wire;
    transmit.state = &capture;
    CHECK(ip4_frag(source, &transmit, NULL) == ERR_OK);
    CHECK(capture.count == 2);
    pbuf_free(source);
    CHECK(frees == 0);
    pbuf_free(capture.packets[0]);
    CHECK(frees == 0);
    pbuf_free(capture.packets[1]);
    CHECK(frees == 1);
    CHECK(lwip_stats.memp[MEMP_FRAG_PBUF]->used == 0);
    return 0;
}

struct test_case { const char *name; int (*run)(void); };
static const struct test_case cases[] = {
    {"normal", normal}, {"reverse_duplicate", reverse_duplicate},
    {"covered_range_duplicate", covered_range_duplicate},
    {"contiguous_range_duplicate", contiguous_range_duplicate},
    {"duplicate_declares_final", duplicate_declares_final},
    {"different_protocol", different_protocol},
    {"different_interface", different_interface},
    {"partial_overlap", partial_overlap}, {"overlap_same_start", overlap_same_start},
    {"conflicting_final", conflicting_final},
    {"fragment_beyond_final", fragment_beyond_final},
    {"missing_fragment_timeout", missing_fragment_timeout},
    {"pbuf_pressure", pbuf_pressure}, {"object_pressure", object_pressure},
    {"duplicate_at_capacity", duplicate_at_capacity},
    {"overlap_at_capacity", overlap_at_capacity},
    {"cleanup_interface", cleanup_interface},
    {"cleanup_external_reference", cleanup_external_reference},
    {"maximum_forward", maximum_forward}, {"maximum_reverse", maximum_reverse},
    {"fragmenter_reference_cleanup", fragmenter_reference_cleanup}
};

int main(int argc, char **argv)
{
    unsigned ran = 0, failed = 0;
    stats_init();
    mem_init();
    memp_init();
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) {
            continue;
        }
        ++ran;
        int status = cases[i].run();
        if (status != 0) {
            printf("FAIL %s\n", cases[i].name);
            return 1;
        }
        expire_all();
        if (live_wire_buffers != 0 || lwip_stats.mem.used != 0 ||
            lwip_stats.memp[MEMP_REASSDATA]->used != 0 ||
            lwip_stats.memp[MEMP_FRAG_PBUF]->used != 0) {
            fprintf(stderr, "%s: resources remain after final cleanup\n", cases[i].name);
            status = 1;
        }
        printf("%s %s\n", status ? "FAIL" : "PASS", cases[i].name);
        failed += status != 0;
    }
    return ran == 0 || failed != 0;
}
