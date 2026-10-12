#ifndef __NETWORK_UTILS_H
#define __NETWORK_UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>

#ifdef __cplusplus
extern "C"
{
#endif

    ssize_t readv_full(int fd, const struct ::iovec *iov, int iovcnt);

    ssize_t writev_all(int fd, const struct ::iovec *iov, int iovcnt);

#ifdef __cplusplus
}
#endif

namespace network
{

    size_t iovec_total_len(const struct ::iovec *iov, uint32_t iovcnt);

    int alloc_single_iovec(struct ::iovec **iov, size_t len);

    void free_single_iovec(struct ::iovec **iov);
} // namespace network

#endif // __NETWORK_UTILS_H