
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
 * fexit hands over both plus the byte count in one trampoline call. */
struct
{
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, INFO_BUFFER_SIZE * 1024);
} rb SEC(".maps");

/* recv() passes a single flat buffer: ITER_UBUF on 6.0+ kernels, a one entry
 * iovec before that. By fexit time the iterator has already been advanced over
 * the bytes the kernel copied, so back `copied` out of iov_offset again. */
static __always_inline const char *recv_user_buf(struct msghdr *msg, __u32 copied)
{
    struct iov_iter *iter = &msg->msg_iter;
    const char *base = NULL;
    __u64 off = BPF_CORE_READ(iter, iov_offset);

    if (off < copied)
        return NULL;

    if (bpf_core_enum_value_exists(enum iter_type, ITER_UBUF) &&
        BPF_CORE_READ(iter, iter_type) == bpf_core_enum_value(enum iter_type, ITER_UBUF))
    {
        base = (const char *)BPF_CORE_READ(iter, ubuf);
    }
    else if (bpf_core_field_exists(iter->__iov))
    {
        /* a fully drained iovec leaves __iov one past the caller's array */
        if (BPF_CORE_READ(iter, nr_segs) == 0)
            return NULL;

        base = (const char *)BPF_CORE_READ(iter, __iov, iov_base);
    }

    if (!base)
        return NULL;

    return base + (off - copied);
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

        struct SliceInfo *info = bpf_ringbuf_reserve(&rb, sizeof(struct SliceInfo), 0);
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

/* Only sk and msg are declared: tcp_recvmsg lost its nonblock parameter in 5.19,
 * so the return value is fetched by helper instead of by fixed argument slot. */
SEC("fexit/tcp_recvmsg")
int BPF_PROG(handle_tcp_recvmsg, struct sock *sk, struct msghdr *msg)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();

    __u32 pid = pid_tgid >> 32;

    if (target_pid != 0 && pid != target_pid)
        return 0;

    if (!sk || !msg)
        return 0;

    __u64 raw_ret = 0;

    if (bpf_get_func_ret(ctx, &raw_ret))
        return 0;

    int ret = (int)raw_ret;

    /* EAGAIN on a coroutine socket, or a peer shutdown */
    if (ret <= 0)
        return 0;

    if (BPF_CORE_READ(sk, __sk_common.skc_family) != AF_INET)
        return 0;

    const char *base = recv_user_buf(msg, (__u32)ret);
    if (!base)
        return 0;

    __be32 saddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    __u16 sport = bpf_ntohs(BPF_CORE_READ(sk, __sk_common.skc_dport));

    submit_slices(make_addr_port_key(saddr, sport), base, (__u32)ret);

    return 0;
}
