#include "coroutine_server.h"
#include "allocator.h"
#include "status.h"

namespace hpc_coroutine
{
    void server(int fd);

    int TcpServers::init()
    {
        for (int i = 0; i < PORT_NUM; i++)
        {
            int sockfd = init_server(_port + i);
            if (sockfd == -1)
            {
                perror("init_server");
            }
            _fd_list[i] = sockfd;
        }
        return 0;
    }

    int TcpServers::start_eventloop()
    {
        for (int i = 0; i < PORT_NUM; i++)
        {
            if (_fd_list[i] != -1)
                hpc_coroutine::CoroutineSched::get_coroutine_sched()->create_coroutine(server, _fd_list[i]);
        }

        hpc_coroutine::CoroutineSched::get_coroutine_sched()->run();
        return 0;
    }

    int TcpServers::init_server(uint16_t port)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            perror("error socket");
            return -1;
        }

        struct sockaddr_in local, remote;
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        local.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&local, sizeof(struct sockaddr_in)) == -1)
        {
            perror("error bind");
            close(fd);
            return -1;
        }

        if (listen(fd, 20) == -1)
        {
            perror("error listen");
            close(fd);
            return -1;
        }

        return fd;
    }

    void server_process(int fd)
    {
        int ret = 0;

        while (1)
        {
            // process header
            struct network::StatusM status;
            struct kv_protocal::KvHeader header;
            char *rbuf;
            char *wbuf;
            memset(&header, 0, kv_protocal::HEADER_SIZE);
            memset(&status, 0, sizeof(status));

            int count = recv(fd, &header, kv_protocal::HEADER_SIZE, 0);
            if (count == 0)
            {
                close(fd);
                break;
            }
            else if (count < 0)
            {
                perror("error recv");
                close(fd);
                break;
            }

            if (count != kv_protocal::HEADER_SIZE)
            {
                perror("Corrupted header");
                close(fd);
                break;
            }

            if (kv_protocal::KvStoreProtocal::instance().process_header(&status, &header) != 0)
            {
                perror("Corrupted header");
                close(fd);
                break;
            }

            rbuf = (char *)allocator::kv_malloc(status.buffer_size + 1);
            if (rbuf == nullptr)
            {
                perror("Error allocate");
                close(fd);
                break;
            }

            int rbuf_size = recv(fd, rbuf, status.buffer_size, 0);
            if (rbuf_size == 0)
            {
                close(fd);
                break;
            }
            else if (rbuf_size < 0)
            {
                perror("error recv");
                close(fd);
                break;
            }

            if (rbuf_size != status.buffer_size)
            {
                perror("corrupted recv buffer");
                close(fd);
                break;
            }

            rbuf[status.buffer_size] = '\0';

            int wbuf_size = kv_protocal::KvStoreProtocal::instance().process_body(&status, rbuf, status.buffer_size, &wbuf);
            if (wbuf_size < 0)
            {
                perror("Error handling body");
                close(fd);
                break;
            }

            count = send(fd, wbuf, kv_protocal::HEADER_SIZE + wbuf_size, 0);
            if (count <= 0)
            {
                perror("Error send");
                close(fd);
                break;
            }
        }
    }

    void server(int fd)
    {
        struct sockaddr_in remote;

        struct timeval tv_begin;
        gettimeofday(&tv_begin, NULL);

        while (1)
        {
            socklen_t len = sizeof(struct sockaddr_in);
            int cli_fd = accept(fd, (struct sockaddr *)&remote, &len);
#ifdef TIMER
            if (cli_fd % 1000 == 999)
            {

                struct timeval tv_cur;
                memcpy(&tv_cur, &tv_begin, sizeof(struct timeval));

                gettimeofday(&tv_begin, NULL);
                int time_used = TIME_SUB_MS(tv_begin, tv_cur);

                printf("client fd : %d, time_used: %d\n", cli_fd, time_used);
            }
#endif
            // printf("new client comming\n");
            hpc_coroutine::CoroutineSched::get_coroutine_sched()->create_coroutine(server_process, cli_fd);
        }
    }
}