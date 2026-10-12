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

    // Mutable copy because we adjust iov_base/iov_len on short reads. readv()
    // accepts at most IOV_MAX segments per call, so process the input in
    // windows of at most IOV_MAX and drain each window fully. This supports an
    // arbitrary iovcnt with no heap allocation; when iovcnt <= IOV_MAX the loop
    // runs exactly once (identical to the single-window path).
    struct ::iovec local_iov[IOV_MAX];

    ssize_t total_read = 0;

    for (int base = 0; base < iovcnt;)
    {
        int cnt = iovcnt - base;
        if (cnt > IOV_MAX)
            cnt = IOV_MAX;

        std::memcpy(local_iov, iov + base, sizeof(struct ::iovec) * cnt);

        int cur = 0;
        while (cur < cnt)
        {
            ssize_t n = readv(fd, &local_iov[cur], cnt - cur);

            if (n > 0)
            {
                total_read += n;

                while (cur < cnt && n >= static_cast<ssize_t>(local_iov[cur].iov_len))
                {
                    n -= local_iov[cur].iov_len;
                    cur++;
                }

                if (cur < cnt && n > 0)
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

        base += cnt;
    }

    return total_read;
}

ssize_t writev_all(int fd, const struct ::iovec *iov, int iovcnt)
{
    if (fd < 0 || iov == nullptr || iovcnt <= 0)
        return -1;

    // Mutable copy because we adjust iov_base/iov_len on short writes. writev()
    // accepts at most IOV_MAX segments per call, so process the input in
    // windows of at most IOV_MAX and drain each window fully. This supports an
    // arbitrary iovcnt with no heap allocation; when iovcnt <= IOV_MAX the loop
    // runs exactly once (identical to the single-window path).
    struct ::iovec local_iov[IOV_MAX];

    ssize_t total_written = 0;

    for (int base = 0; base < iovcnt;)
    {
        int cnt = iovcnt - base;
        if (cnt > IOV_MAX)
            cnt = IOV_MAX;

        std::memcpy(local_iov, iov + base, sizeof(struct ::iovec) * cnt);

        int cur = 0;
        while (cur < cnt)
        {
            ssize_t n = writev(fd, &local_iov[cur], cnt - cur);

            if (n > 0)
            {
                total_written += n;

                while (cur < cnt && n >= static_cast<ssize_t>(local_iov[cur].iov_len))
                {
                    n -= local_iov[cur].iov_len;
                    cur++;
                }

                if (cur < cnt && n > 0)
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

        base += cnt;
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