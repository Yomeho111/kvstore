#include "hpc_socket.h"
#include <sys/epoll.h>
#include <poll.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
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