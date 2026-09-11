
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_endian.h>

/* a vmlinux.h dumped from a live kernel already typedefs wchar_t, so clang's
 * stddef.h (pulled in by delta.h) must not redefine it */
#ifndef _WCHAR_T
#define _WCHAR_T
#endif

#include "delta.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

const volatile pid_t target_pid = 0;

#define AF_INET 2

/* One recv() never asks for more than RESP_RECV_BUF_SIZE bytes, so this many
 * INFO_BUFFER_SIZE slices always carry a whole call up to userspace. */
#define MAX_SLICES (RESP_RECV_BUF_SIZE / INFO_BUFFER_SIZE)

/* The server only ever reads with recv(), so tcp_recvmsg() is the single place
 * that sees both the peer address (struct sock) and the destination buffer.
 * The entry probe records them, the return probe knows how many bytes landed. */
struct RecvCall
{
    struct sock *sk;
    const char *base;
};

struct
{
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u64);
    __type(value, struct RecvCall);
} active_recv SEC(".maps");

struct
{
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 4 * 1024 * 1024);
} rb SEC(".maps");

/* recv() passes a single flat buffer: ITER_UBUF on 6.0+ kernels, a one entry
 * iovec before that. Must be resolved on entry, iov_offset advances as the
 * kernel copies. */
static __always_inline const char *recv_user_buf(struct msghdr *msg)
{
    struct iov_iter *iter = &msg->msg_iter;
    const char *base = NULL;

    if (bpf_core_enum_value_exists(enum iter_type, ITER_UBUF) &&
        BPF_CORE_READ(iter, iter_type) == bpf_core_enum_value(enum iter_type, ITER_UBUF))
    {
        base = (const char *)BPF_CORE_READ(iter, ubuf);
    }
    else if (bpf_core_field_exists(iter->__iov))
    {
        const struct iovec *iov = BPF_CORE_READ(iter, __iov);

        base = (const char *)BPF_CORE_READ(iov, iov_base);
    }

    if (!base)
        return NULL;

    return base + BPF_CORE_READ(iter, iov_offset);
}

static __always_inline void submit_slices(__u64 addr_key, const char *base, __u32 total)
{
    __u32 off = 0;

    for (int i = 0; i < MAX_SLICES; i++)
    {
        if (off >= total)
            return;

        __u32 chunk = total - off;

        if (chunk > INFO_BUFFER_SIZE)
            chunk = INFO_BUFFER_SIZE;

        struct SliceInfo *info = bpf_ringbuf_reserve(&rb, sizeof(*info), 0);
        if (!info)
            return;

        info->addr_sport = addr_key;
        info->size = chunk;

        if (bpf_probe_read_user(info->buf, chunk, base + off))
        {
            bpf_ringbuf_discard(info, 0);
            return;
        }

        bpf_ringbuf_submit(info, 0);
        off += chunk;
    }
}

SEC("kprobe/tcp_recvmsg")
int BPF_KPROBE(handle_tcp_recvmsg, struct sock *sk, struct msghdr *msg)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();

    __u32 pid = pid_tgid >> 32;

    if (target_pid != 0 && pid != target_pid)
        return 0;

    if (!sk || !msg)
        return 0;

    struct RecvCall call = {
        .sk = sk,
        .base = recv_user_buf(msg),
    };

    if (!call.base)
        return 0;

    bpf_map_update_elem(&active_recv, &pid_tgid, &call, BPF_ANY);

    return 0;
}

SEC("kretprobe/tcp_recvmsg")
int BPF_KRETPROBE(handle_tcp_recvmsg_ret, int ret)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();

    struct RecvCall *call = bpf_map_lookup_elem(&active_recv, &pid_tgid);
    if (!call)
        return 0;

    struct sock *sk = call->sk;
    const char *base = call->base;

    bpf_map_delete_elem(&active_recv, &pid_tgid);

    /* EAGAIN on a coroutine socket, or a peer shutdown */
    if (ret <= 0)
        return 0;

    if (BPF_CORE_READ(sk, __sk_common.skc_family) != AF_INET)
        return 0;

    __be32 saddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    __u16 sport = bpf_ntohs(BPF_CORE_READ(sk, __sk_common.skc_dport));

    submit_slices(make_addr_port_key(saddr, sport), base, (__u32)ret);

    return 0;
}
