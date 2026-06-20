#include "network_utils.h"

#include <sys/socket.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <cstring>

#include "allocator.h"

ssize_t readv_full(int fd, const struct ::iovec *iov, int iovcnt)
{
    if (fd < 0 || iov == nullptr || iovcnt <= 0)
        return -1;

    // Make a mutable copy because we need to adjust iov_base/iov_len
    struct ::iovec local_iov[IOV_MAX];

    if (iovcnt > IOV_MAX)
    {
        errno = EINVAL;
        return -1;
    }

    std::memcpy(local_iov, iov, sizeof(struct ::iovec) * iovcnt);

    ssize_t total_read = 0;
    int cur = 0;

    while (cur < iovcnt)
    {
        ssize_t n = readv(fd, &local_iov[cur], iovcnt - cur);

        if (n > 0)
        {
            total_read += n;

            while (cur < iovcnt && n >= static_cast<ssize_t>(local_iov[cur].iov_len))
            {
                n -= local_iov[cur].iov_len;
                cur++;
            }

            if (cur < iovcnt && n > 0)
            {
                local_iov[cur].iov_base =
                    static_cast<char *>(local_iov[cur].iov_base) + n;
                local_iov[cur].iov_len -= n;
            }

            continue;
        }
        else if (n == 0)
        {
            // Peer closed connection before all expected bytes were read
            return total_read;
        }
        else if (errno == EINTR)
        {
            continue;
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // Non-blocking socket has no more data right now
            return total_read;
        }
        else
            return -1;
    }

    return total_read;
}

ssize_t writev_all(int fd, const struct ::iovec *iov, int iovcnt)
{
    if (fd < 0 || iov == nullptr || iovcnt <= 0)
        return -1;

    struct ::iovec local_iov[IOV_MAX];

    if (iovcnt > IOV_MAX)
    {
        errno = EINVAL;
        return -1;
    }

    std::memcpy(local_iov, iov, sizeof(struct ::iovec) * iovcnt);

    ssize_t total_written = 0;
    int cur = 0;

    while (cur < iovcnt)
    {
        ssize_t n = writev(fd, &local_iov[cur], iovcnt - cur);

        if (n > 0)
        {
            total_written += n;

            while (cur < iovcnt && n >= static_cast<ssize_t>(local_iov[cur].iov_len))
            {
                n -= local_iov[cur].iov_len;
                cur++;
            }

            if (cur < iovcnt && n > 0)
            {
                local_iov[cur].iov_base =
                    static_cast<char *>(local_iov[cur].iov_base) + n;
                local_iov[cur].iov_len -= n;
            }

            continue;
        }
        else if (n == 0)
        {
            // writev returning 0 usually means no progress
            errno = EPIPE;
            return -1;
        }
        else if (errno == EINTR)
        {
            continue;
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // Non-blocking socket cannot write more right now
            return total_written;
        }
        else
            return -1;
    }

    return total_written;
}

namespace network
{

    size_t iovec_total_len(const struct iovec *iov, uint32_t iovcnt)
    {
        size_t total = 0;
        for (uint32_t i = 0; i < iovcnt; i++)
        {
            total += iov[i].iov_len;
        }
        return total;
    }

    int alloc_single_iovec(struct iovec **iov, size_t len)
    {
        *iov = static_cast<struct iovec *>(allocator::kv_malloc(sizeof(struct iovec)));
        if (*iov == nullptr)
            return -1;

        (*iov)[0].iov_base = allocator::kv_malloc(len);
        if ((*iov)[0].iov_base == nullptr)
        {
            allocator::kv_free(*iov);
            *iov = nullptr;
            return -1;
        }

        (*iov)[0].iov_len = len;
        return 0;
    }

    void free_single_iovec(struct iovec **iov)
    {
        if (iov == nullptr || *iov == nullptr)
            return;

        allocator::kv_free((*iov)[0].iov_base);
        (*iov)[0].iov_base = nullptr;
        allocator::kv_free(*iov);
        *iov = nullptr;
    }
} // namespace network