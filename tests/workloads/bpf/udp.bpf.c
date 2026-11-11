#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} udp_rule SEC(".maps");

const volatile __u32 rules_version = 7;

SEC("socket")
int udp_policy(struct __sk_buff *skb)
{
    __u32 key = 0;
    __u32 *allow = bpf_map_lookup_elem(&udp_rule, &key);
    if (rules_version != 7 || !allow || !*allow)
        return 0;
    return skb->len;
}
char LICENSE[] SEC("license") = "GPL";
