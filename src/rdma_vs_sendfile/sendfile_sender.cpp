// sendfile(2) side of the rdma-vs-sendfile benchmark: streams a file over a TCP
// connection without copying the payload into user space.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>

#define MAGIC 0x234324
#define CHUNK_SIZE ((size_t)4 * 1024 * 1024)
#define CTRL_DONE 2

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

static int connect_peer(const char *ip, uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        fprintf(stderr, "socket: %s\n", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (1 != inet_pton(AF_INET, ip, &dst.sin_addr))
    {
        fprintf(stderr, "'%s' is not a valid IPv4 address\n", ip);
        close(sock);
        return -1;
    }

    if (0 != connect(sock, (struct sockaddr *)&dst, sizeof(dst)))
    {
        fprintf(stderr, "connect %s:%u: %s\n", ip, port, strerror(errno));
        close(sock);
        return -1;
    }

    return sock;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <ip> <port> [file]\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    unsigned long port = strtoul(argv[2], NULL, 10);
    if (port == 0 || port > 65535)
    {
        fprintf(stderr, "'%s' is not a valid port\n", argv[2]);
        return 1;
    }
    const char *path = (argc > 3) ? argv[3] : "./bigfile.dat";

    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 1;
    }

    struct stat st;
    if (0 != fstat(fd, &st))
    {
        fprintf(stderr, "fstat %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    size_t total = (size_t)st.st_size;

    int sock = connect_peer(ip, (uint16_t)port);
    if (sock < 0)
    {
        close(fd);
        return 1;
    }

    printf("connected to %s:%lu, sending %s (%zu bytes)\n", ip, port, path, total);
    fflush(stdout);

    int rc = 1;

    struct file_info fi;
    memset(&fi, 0, sizeof(fi));
    fi.magic = MAGIC;
    fi.total = total;
    if (0 != write_full(sock, &fi, sizeof(fi)))
    {
        fprintf(stderr, "sending the header: %s\n", strerror(errno));
        goto out;
    }

    {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

        off_t off = 0;
        while ((size_t)off < total)
        {
            size_t remaining = total - (size_t)off;
            size_t count = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;

            ssize_t n = sendfile(sock, fd, &off, count);
            if (n < 0)
            {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                fprintf(stderr, "sendfile: %s\n", strerror(errno));
                goto out;
            }
            if (n == 0)
            {
                fprintf(stderr, "sendfile returned 0 with %zu bytes left\n", remaining);
                goto out;
            }
        }

        // The receiver acks once the last byte has been written to its file.
        char ack = 0;
        if (0 != read_full(sock, &ack, 1) || ack != CTRL_DONE)
        {
            fprintf(stderr, "no completion ack from the receiver\n");
            goto out;
        }

        report("sendfile send", total, elapsed_s(start));
        rc = 0;
    }

out:
    close(sock);
    close(fd);
    return rc;
}
