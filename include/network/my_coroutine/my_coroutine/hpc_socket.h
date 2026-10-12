#ifndef __HPC_SOCKET_H
#define __HPC_SOCKET_H
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef int (*socket_t)(int domain, int type, int protocol);
    extern socket_t socket_f;

    typedef int (*connect_t)(int, const struct sockaddr *, socklen_t);
    extern connect_t connect_f;

    typedef ssize_t (*recv_t)(int sockfd, void *buf, size_t len, int flags);
    extern recv_t recv_f;

    typedef ssize_t (*send_t)(int sockfd, const void *buf, size_t len, int flags);
    extern send_t send_f;

    typedef int (*accept_t)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
    extern accept_t accept_f;

    typedef int (*close_t)(int);
    extern close_t close_f;

    int socket(int domain, int type, int protocol);

    ssize_t recv(int fd, void *buf, size_t len, int flags);

    ssize_t send(int fd, const void *buf, size_t len, int flags);

    int accept(int fd, struct sockaddr *addr, socklen_t *len);

    int close(int fd);

    int connect(int fd, const struct sockaddr *addr, socklen_t addrlen);

    ssize_t readv_full(int fd, const struct ::iovec *iov, int iovcnt);

    ssize_t writev_all(int fd, const struct ::iovec *iov, int iovcnt);

#ifdef __cplusplus
}
#endif

#endif // __HPC_SOCKET_H