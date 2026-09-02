#include "vmlinux.h"

#include <stddef.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "delta.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

struct
{
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} rb SEC(".maps");

SEC("uprobe")
int BPF_UPROBE(kv_replication_commit_enter, int sequence)
{
    struct DeltaEvent *e;

    e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
    if (!e)
        return 0;

    __u64 pid_tgid = bpf_get_current_pid_tgid();

    e->pid = pid_tgid >> 32;
    e->sequence = sequence;

    bpf_ringbuf_submit(e, 0);

    return 0;
}
