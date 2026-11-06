#include "lwip/mem.h"
#include <assert.h>

static void shared_payload_cases(void)
{
    mem_size_t before = lwip_stats.mem.used;
    struct pbuf *source = pbuf_alloc(PBUF_RAW, 4116, PBUF_RAM);
    struct pbuf *second = pbuf_alloc(PBUF_RAW, 4096, PBUF_RAM);
    assert(source && second);
    pbuf_cat(source, second);
    for (u16_t i = 0; i < source->tot_len; i++) pbuf_put_at(source, i, (u8_t)i);
    struct pbuf *view = pbuf_clone_shared(PBUF_RAW, source, 20);
    assert(view && view->len == 20 && view->tot_len == 8212);
    assert(view->next->payload == (u8_t *)source->payload + 20 &&
           view->next->next->payload == second->payload);
    assert(source->ref == 1 && second->ref == 1);
    /* Another transmission changes headers, never the published payload. */
    pbuf_put_at(source, 0, 0xee);
    struct pbuf *retransmit = pbuf_clone_shared(PBUF_RAW, source, 20);
    assert(retransmit && pbuf_get_at(view, 0) == 0 && pbuf_get_at(retransmit, 0) == 0xee);
    assert(source->ref == 1 && second->ref == 1);
    /* Sender shrink/ACK cannot reclaim bytes still visible in either RX chain. */
    pbuf_realloc(source, 37);
    pbuf_free(source);
    view = pbuf_free_header(view, 20);
    assert(view && view->tot_len == 8192);
    pbuf_realloc(view, 4129);
    for (u16_t i = 0; i < view->tot_len; i++) assert(pbuf_get_at(view, i) == (u8_t)(i + 20));
    for (u16_t i = 20; i < retransmit->tot_len; i++) assert(pbuf_get_at(retransmit, i) == (u8_t)i);
    pbuf_free(view);
    pbuf_free(retransmit);
    assert(lwip_stats.mem.used == before);

    /* External storage may change immediately after the sender is finished. */
    unsigned char external[128];
    for (unsigned i = 0; i < sizeof(external); i++) external[i] = (u8_t)i;
    source = pbuf_alloc_reference(external, sizeof(external), PBUF_REF);
    assert(source);
    view = pbuf_clone_shared(PBUF_RAW, source, 20);
    assert(view && view->len == sizeof(external) && !view->next);
    for (unsigned i = 0; i < sizeof(external); i++) external[i] = 0;
    pbuf_free(source);
    for (u16_t i = 0; i < view->tot_len; i++) assert(pbuf_get_at(view, i) == (u8_t)i);
    pbuf_free(view);
    assert(lwip_stats.mem.used == before);

    /* A constrained real heap cannot fit the complete descriptor group.
     * Allocation failure must leave every source reference unchanged. */
    source = pbuf_alloc(PBUF_RAW, 128, PBUF_RAM);
    assert(source);
    for (unsigned i = 1; i < 16; i++) {
        second = pbuf_alloc(PBUF_RAW, 128, PBUF_RAM);
        assert(second);
        pbuf_cat(source, second);
    }
    static void *held[MEM_SIZE / 256 + 1];
    unsigned count = 0;
    while (count < sizeof(held) / sizeof(held[0]) && (held[count] = mem_malloc(256))) count++;
    assert(count && count < sizeof(held) / sizeof(held[0]));
    mem_free(held[--count]);
    mem_size_t occupied = lwip_stats.mem.used;
    view = pbuf_clone_shared(PBUF_RAW, source, 20);
    assert(!view && lwip_stats.mem.used == occupied);
    for (second = source; second; second = second->next)
        assert(second->ref == 1 && second->storage_refs == 1);
    while (count) mem_free(held[--count]);
    pbuf_free(source);
    assert(lwip_stats.mem.used == before);
}
