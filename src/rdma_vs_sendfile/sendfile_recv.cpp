// sendfile(2) side of the rdma-vs-sendfile benchmark: receives a file from a TCP
// connection. There is no receive-side counterpart to sendfile, so the payload
// is copied through a user-space buffer, which is exactly the cost RDMA avoids.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>

#define MAGIC 0x234324
#define CHUNK_SIZE ((size_t)4 * 1024 * 1024)
#define CTRL_DONE 2
#define MAX_FILE_SIZE ((size_t)64 * 1024 * 1024 * 1024)

struct file_info
{
    int magic;
    size_t total;
};

static double elapsed_s(const std::chrono::steady_clock::time_point &start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

static void report(const char *tag, size_t bytes, double sec)
{
    double mb = (double)bytes / (1024.0 * 1024.0);
    double rate = sec > 0.0 ? mb / sec : 0.0;
    double gbps = sec > 0.0 ? (double)bytes * 8.0 / sec / 1e9 : 0.0;
    printf("[%s] %.2f MB in %.3f s -> %.2f MB/s (%.2f Gb/s)\n", tag, mb, sec, rate, gbps);
    fflush(stdout);
}

static int write_full(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t done = 0;
    while (done < len)
    {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t done = 0;
    while (done < len)
    {
        ssize_t n = read(fd, p + done, len - done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int accept_peer(const char *ip, uint16_t port)
{
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0)
    {
        fprintf(stderr, "socket: %s\n", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (1 != inet_pton(AF_INET, ip, &addr.sin_addr))
    {
        fprintf(stderr, "'%s' is not a valid IPv4 address\n", ip);
        close(listen_sock);
        return -1;
    }

    if (0 != bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)))
    {
        fprintf(stderr, "bind %s:%u: %s\n", ip, port, strerror(errno));
        close(listen_sock);
        return -1;
    }

    if (0 != listen(listen_sock, 1))
    {
        fprintf(stderr, "listen: %s\n", strerror(errno));
        close(listen_sock);
        return -1;
    }

    printf("waiting for a sendfile sender on %s:%u\n", ip, port);
    fflush(stdout);

    int sock = accept(listen_sock, NULL, NULL);
    if (sock < 0)
        fprintf(stderr, "accept: %s\n", strerror(errno));

    close(listen_sock);

    if (sock >= 0)
    {
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    return sock;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <ip> <port> [out-file]\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    unsigned long port = strtoul(argv[2], NULL, 10);
    if (port == 0 || port > 65535)
    {
        fprintf(stderr, "'%s' is not a valid port\n", argv[2]);
        return 1;
    }
    const char *path = (argc > 3) ? argv[3] : "./bigfile.sendfile.out";

    int sock = accept_peer(ip, (uint16_t)port);
    if (sock < 0)
        return 1;

    int rc = 1;
    int fd = -1;
    char *buf = NULL;
    size_t total = 0;

    {
        struct file_info fi;
        if (0 != read_full(sock, &fi, sizeof(fi)))
        {
            fprintf(stderr, "reading the header: %s\n", strerror(errno));
            goto out;
        }
        if (fi.magic != MAGIC)
        {
            fprintf(stderr, "bad header magic 0x%x\n", fi.magic);
            goto out;
        }
        if (fi.total > MAX_FILE_SIZE)
        {
            fprintf(stderr, "announced file size %zu exceeds the %zu byte limit\n",
                    fi.total, (size_t)MAX_FILE_SIZE);
            goto out;
        }
        total = fi.total;
    }

    printf("receiving %zu bytes into %s\n", total, path);
    fflush(stdout);

    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        goto out;
    }
    if (total > 0 && 0 != ftruncate(fd, (off_t)total))
    {
        fprintf(stderr, "ftruncate %s: %s\n", path, strerror(errno));
        goto out;
    }

    buf = (char *)malloc(CHUNK_SIZE);
    if (!buf)
        goto out;

    {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

        size_t got = 0;
        while (got < total)
        {
            size_t remaining = total - got;
            size_t want = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;

            ssize_t n = read(sock, buf, want);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                fprintf(stderr, "read: %s\n", strerror(errno));
                goto out;
            }
            if (n == 0)
            {
                fprintf(stderr, "peer closed with %zu bytes left\n", remaining);
                goto out;
            }

            if (0 != write_full(fd, buf, (size_t)n))
            {
                fprintf(stderr, "write %s: %s\n", path, strerror(errno));
                goto out;
            }
            got += (size_t)n;
        }

        double transfer_s = elapsed_s(start);

        char ack = CTRL_DONE;
        if (0 != write_full(sock, &ack, 1))
        {
            fprintf(stderr, "sending the ack: %s\n", strerror(errno));
            goto out;
        }

        report("sendfile recv", total, transfer_s);

        std::chrono::steady_clock::time_point flush_start = std::chrono::steady_clock::now();
        fsync(fd);
        report("flush", total, elapsed_s(flush_start));
        rc = 0;
    }

out:
    free(buf);
    if (fd >= 0)
        close(fd);
    close(sock);
    return rc;
}
