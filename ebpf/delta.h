#ifndef __DELTA_H
#define __DELTA_H

// bpf_helpers.h is BPF-target only: its __type() macro collides with libstdc++
// and bpf_helper_defs.h is not valid C++.
#ifdef __bpf__
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#else
#include <linux/types.h>
#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif
#endif

#include <stddef.h>

#define RESP_RECV_BUF_SIZE 32768
#define INFO_BUFFER_SIZE 4096

static __always_inline __u64
make_addr_port_key(__be32 saddr, __u16 sport)
{
    return ((__u64)(__u32)saddr << 16) |
           (__u64)sport;
}

static __always_inline void
parse_addr_port_key(__u64 key, __be32 *saddr, __u16 *sport)
{
    *saddr = (__be32)((key >> 16) & 0xffffffffULL);
    *sport = (__u16)(key & 0xffffULL);
}

struct SliceInfo
{
    __u64 addr_sport;
    size_t size;
    char buf[INFO_BUFFER_SIZE];
};

#endif // __DELTA_H