#include "hpc_socket.h"
#include <sys/epoll.h>
#include <poll.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <cstring>
#include "hpc_coroutine.h"

socket_t socket_f = nullptr;
connect_t connect_f = nullptr;
recv_t recv_f = nullptr;
send_t send_f = nullptr;
accept_t accept_f = nullptr;
close_t close_f = nullptr;

static void init_hook()
{
    socket_f = (socket_t)dlsym(RTLD_NEXT, "socket");

    recv_f = (recv_t)dlsym(RTLD_NEXT, "recv");
    send_f = (send_t)dlsym(RTLD_NEXT, "send");

    accept_f = (accept_t)dlsym(RTLD_NEXT, "accept");
    close_f = (close_t)dlsym(RTLD_NEXT, "close");
    connect_f = (connect_t)dlsym(RTLD_NEXT, "connect");
}

int socket(int domain, int type, int protocol)
{
    if (!socket_f)
        init_hook();

    int fd = socket_f(domain, type, protocol);
    if (fd == -1)
    {
        perror("Failed to create a new socket");
        return -1;
    }
    int ret = fcntl(fd, F_SETFL, O_NONBLOCK);
    if (ret == -1)
    {
        close(ret);
        return -1;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

    return fd;
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    if (!recv_f)
        init_hook();

    struct pollfd fds;
    fds.fd = fd;
    fds.events = POLLIN | POLLERR | POLLHUP;

    int ret = hpc_coroutine::CoroutineSched::get_coroutine_sched()->poll_inner(&fds, 1);
    if (ret == -1)
    {
        perror("Error recv");
        return -1;
    }

    ret = recv_f(fd, buf, len, flags);
    if (ret <= 0)
    {
        if (ret == 0)
        {
            errno = 0;
            return 0;
        }
        if (errno == EAGAIN)
            return ret;
        if (errno == ECONNRESET)
        {
            perror("reset by peer");
            return -1;
        }
    }
    return ret;
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    if (!send_f)
        init_hook();

    size_t sent = 0;

    int ret = send_f(fd, ((char *)buf) + sent, len - sent, flags);
    if (ret == 0)
        return ret;
    if (ret > 0)
        sent += ret;

    while (sent < len)
    {
        struct pollfd fds;
        fds.fd = fd;
        fds.events = POLLOUT | POLLERR | POLLHUP;

        ret = hpc_coroutine::CoroutineSched::get_coroutine_sched()->poll_inner(&fds, 1);
        if (ret == -1)
        {
            perror("Error send");
            return -1;
        }
        ret = send_f(fd, ((char *)buf) + sent, len - sent, flags);
        // printf("send --> len : %d\n", ret);
        if (ret <= 0)
        {
            break;
        }
        sent += ret;
    }

    if (ret <= 0 && sent == 0)
        return ret;

    return sent;
}

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    if (!accept_f)
        init_hook();

    int sockfd = -1;

    while (1)
    {
        struct pollfd fds;
        fds.fd = fd;
        fds.events = POLLIN | POLLERR | POLLHUP;
        int ret = hpc_coroutine::CoroutineSched::get_coroutine_sched()->poll_inner(&fds, 1);
        if (ret == -1)
        {
            perror("Error accept");
            return -1;
        }

        sockfd = accept_f(fd, addr, len);
        if (sockfd < 0)
        {
            if (errno == EAGAIN)
            {
                continue;
            }
            else if (errno == ECONNABORTED)
            {
                printf("accept : ECONNABORTED\n");
            }
            else if (errno == EMFILE || errno == ENFILE)
            {
                printf("accept : EMFILE || ENFILE\n");
            }
            return -1;
        }
        else
        {
            break;
        }
    }

    int ret = fcntl(sockfd, F_SETFL, O_NONBLOCK);
    if (ret == -1)
    {
        close(sockfd);
        return -1;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

    return sockfd;
}

int close(int fd)
{
    if (!close_f)
        init_hook();

    return close_f(fd);
}

int connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    if (!connect_f)
        init_hook();

    int ret = 0;

    while (1)
    {

        struct pollfd fds;
        fds.fd = fd;
        fds.events = POLLOUT | POLLERR | POLLHUP;
        ret = hpc_coroutine::CoroutineSched::get_coroutine_sched()->poll_inner(&fds, 1);

        ret = connect_f(fd, addr, addrlen);
        if (ret == 0)
            break;

        if (ret == -1 && (errno == EAGAIN ||
                          errno == EWOULDBLOCK ||
                          errno == EINPROGRESS))
        {
            continue;
        }
        else
        {
            break;
        }
    }

    return ret;
}

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
        struct pollfd fds;
        fds.fd = fd;
        fds.events = POLLIN | POLLERR | POLLHUP;

        int ret = hpc_coroutine::CoroutineSched::get_coroutine_sched()->poll_inner(&fds, 1);
        if (ret == -1)
        {
            perror("Error recv");
            return -1;
        }

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
            continue;
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
        struct pollfd fds;
        fds.fd = fd;
        fds.events = POLLOUT | POLLERR | POLLHUP;

        int ret = hpc_coroutine::CoroutineSched::get_coroutine_sched()->poll_inner(&fds, 1);
        if (ret == -1)
        {
            perror("Error send");
            return -1;
        }

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
            continue;
        }
        else
            return -1;
    }

    return total_written;
}