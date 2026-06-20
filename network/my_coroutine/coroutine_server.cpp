#include "coroutine_server.h"
#include "allocator.h"
#include "status.h"

#include <errno.h>
#include <sys/uio.h>

#define HEARTBEAT_INTERVAL_MS 2000

namespace hpc_coroutine
{
    void server(int fd);
    void slave_process(int fd);
    void slave_run(const char *ip, uint16_t port);

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
        while (1)
        {
            struct network::StatusM status;
            struct kv_protocal::NumHeader num_header;
            struct iovec *r_iovec = nullptr;
            struct iovec *w_iovec = nullptr;
            bool should_close = false;
            size_t header_size = 0;
            int process_count = 0;

            memset(&status, 0, sizeof(status));
            memset(&num_header, 0, kv_protocal::NUM_HEADER_SIZE);
            status.status = network::READ_NUM_REQUEST;

            int ret = recv(fd, &num_header, sizeof(struct kv_protocal::NumHeader), 0);
            if (ret <= 0)
            {
                if (ret < 0)
                    perror("error recv");
                close(fd);
                return;
            }

            if (num_header.num_request == 0)
                continue;

            if (kv_protocal::KvStoreProtocal::instance().process_num_request(&status, num_header.num_request) != 0)
            {
                perror("Processing number of request failure");
                should_close = true;
                goto clean;
            }

            header_size = status.num_request * kv_protocal::HEADER_SIZE;
            status.req_info = (kv_protocal::HeaderInfo *)allocator::kv_malloc(header_size);
            if (status.req_info == nullptr)
            {
                perror("Error Memory allocation");
                goto clean;
            }

            ret = recv(fd, status.req_info, header_size, 0);
            if (ret <= 0)
            {
                if (ret < 0)
                    perror("error recv");
                should_close = true;
                goto clean;
            }

            if (kv_protocal::KvStoreProtocal::instance().process_header(&status, &r_iovec) != 0)
            {
                perror("Processing header failure");
                should_close = true;
                goto clean;
            }

            ret = readv_full(fd, r_iovec, status.num_request);
            if (ret <= 0)
            {
                if (ret < 0)
                    perror("error read");
                should_close = true;
                goto clean;
            }

            process_count = kv_protocal::KvStoreProtocal::instance().process_body(&status, r_iovec, &w_iovec);
            if (process_count < 0)
            {
                perror("Error handling body");
                should_close = true;
                goto clean;
            }

            if (status.status == network::SEND_RESPONSE)
            {
                ret = writev_all(fd, w_iovec, status.w_iovec_size);
                if (ret <= 0)
                {
                    if (ret < 0)
                        perror("Error write");
                    should_close = true;
                    goto clean;
                }
            }

        clean:
            if (status.req_info != nullptr)
            {
                allocator::kv_free(status.req_info);
                status.req_info = nullptr;
            }

            if (r_iovec != nullptr)
            {
                for (uint32_t i = 0; i < status.num_request; i++)
                {
                    if (r_iovec[i].iov_base != nullptr)
                    {
                        allocator::kv_free(r_iovec[i].iov_base);
                        r_iovec[i].iov_base = nullptr;
                    }
                }
                allocator::kv_free(r_iovec);
                r_iovec = nullptr;
            }

            if (w_iovec != nullptr)
            {
                for (uint32_t i = 0; i < status.w_iovec_size; i++)
                {
                    if (w_iovec[i].iov_base != nullptr)
                    {
                        allocator::kv_free(w_iovec[i].iov_base);
                        w_iovec[i].iov_base = nullptr;
                    }
                }
                allocator::kv_free(w_iovec);
                w_iovec = nullptr;
            }

            if (should_close)
            {
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

    void slave_process(int fd)
    {
        while (1)
        {
            struct network::StatusM status;
            struct kv_protocal::NumHeader num_header;
            struct iovec *r_iovec = nullptr;
            struct iovec *w_iovec = nullptr;
            bool should_close = false;
            size_t header_size = 0;
            int process_count = 0;
            int ret = 0;

            memset(&status, 0, sizeof(status));
            memset(&num_header, 0, kv_protocal::NUM_HEADER_SIZE);
            status.status = network::READ_NUM_REQUEST;

            // 1. send a heartbeat packet carrying the current sync_idx
            if (kv_protocal::KvStoreProtocal::instance().construct_heartbeat_packet(&status, &w_iovec) < 0)
            {
                perror("Error handling heartbeat packet");
                should_close = true;
                goto clean;
            }

            ret = writev_all(fd, w_iovec, status.w_iovec_size);
            if (ret <= 0)
            {
                if (ret < 0)
                    perror("Error write heartbeat");
                should_close = true;
                goto clean;
            }

            // free heartbeat w_iovec before reusing it for the response
            if (w_iovec != nullptr)
            {
                for (uint32_t i = 0; i < status.w_iovec_size; i++)
                {
                    if (w_iovec[i].iov_base != nullptr)
                    {
                        allocator::kv_free(w_iovec[i].iov_base);
                        w_iovec[i].iov_base = nullptr;
                    }
                }
                allocator::kv_free(w_iovec);
                w_iovec = nullptr;
            }
            status.w_iovec_size = 0;

            // 2. read the master's sync response
            status.status = network::READ_NUM_REQUEST;
            ret = recv(fd, &num_header, sizeof(struct kv_protocal::NumHeader), 0);
            if (ret <= 0)
            {
                if (ret < 0)
                    perror("error recv");
                should_close = true;
                goto clean;
            }

            if (num_header.num_request == 0)
                goto clean;

            if (kv_protocal::KvStoreProtocal::instance().process_num_request(&status, num_header.num_request) != 0)
            {
                perror("Processing number of request failure");
                should_close = true;
                goto clean;
            }

            header_size = status.num_request * kv_protocal::HEADER_SIZE;
            ret = recv(fd, status.req_info, header_size, 0);
            if (ret <= 0)
            {
                if (ret < 0)
                    perror("error recv");
                should_close = true;
                goto clean;
            }

            if (kv_protocal::KvStoreProtocal::instance().process_header(&status, &r_iovec) != 0)
            {
                perror("Processing header failure");
                should_close = true;
                goto clean;
            }

            ret = readv_full(fd, r_iovec, status.num_request);
            if (ret <= 0)
            {
                if (ret < 0)
                    perror("error read");
                should_close = true;
                goto clean;
            }

            // 3. apply the sync data to the local engine
            process_count = kv_protocal::KvStoreProtocal::instance().process_body(&status, r_iovec, &w_iovec);
            if (process_count < 0)
            {
                perror("Error handling body");
                should_close = true;
                goto clean;
            }

            if (status.status == network::SEND_RESPONSE)
            {
                ret = writev_all(fd, w_iovec, status.w_iovec_size);
                if (ret <= 0)
                {
                    if (ret < 0)
                        perror("Error write");
                    should_close = true;
                    goto clean;
                }
            }

        clean:
            if (status.req_info != nullptr)
            {
                allocator::kv_free(status.req_info);
                status.req_info = nullptr;
            }

            if (r_iovec != nullptr)
            {
                for (uint32_t i = 0; i < status.num_request; i++)
                {
                    if (r_iovec[i].iov_base != nullptr)
                    {
                        allocator::kv_free(r_iovec[i].iov_base);
                        r_iovec[i].iov_base = nullptr;
                    }
                }
                allocator::kv_free(r_iovec);
                r_iovec = nullptr;
            }

            if (w_iovec != nullptr)
            {
                for (uint32_t i = 0; i < status.w_iovec_size; i++)
                {
                    if (w_iovec[i].iov_base != nullptr)
                    {
                        allocator::kv_free(w_iovec[i].iov_base);
                        w_iovec[i].iov_base = nullptr;
                    }
                }
                allocator::kv_free(w_iovec);
                w_iovec = nullptr;
            }

            if (should_close)
            {
                close(fd);
                break;
            }

            // Pace heartbeats and, crucially, yield back to the scheduler so the
            // idle wait happens on the scheduler's main stack (not this
            // coroutine's stack). This avoids busy-looping the master.
            CoroutineSched::get_coroutine_sched()->co_sleep(HEARTBEAT_INTERVAL_MS);
        }
    }

    int TcpSlaveServer::init_client()
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            perror("socket");
            return -1;
        }

        struct sockaddr_in server_addr
        {
        };
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(_port);

        // Convert "127.0.0.1" to binary IP format
        if (inet_pton(AF_INET, _ip, &server_addr.sin_addr) <= 0)
        {
            perror("inet_pton");
            close(fd);
            return -1;
        }

        if (connect(fd, reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr)) < 0)
        {
            perror("connect");
            close(fd);
            return -1;
        }

        return fd;
    }

    // Runs inside a coroutine: the hooked socket()/connect()/recv()/send()
    // yield to the scheduler, so they must NOT be called before the scheduler
    // is running (i.e. not from TcpSlaveServer::init()).
    void slave_run(const char *ip, uint16_t port)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            perror("socket");
            return;
        }

        struct sockaddr_in server_addr
        {
        };
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);

        if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0)
        {
            perror("inet_pton");
            close(fd);
            return;
        }

        if (connect(fd, reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr)) < 0)
        {
            perror("connect");
            close(fd);
            return;
        }

        slave_process(fd);
    }

    int TcpSlaveServer::init()
    {
        // Do NOT connect here: the hooked socket/connect yield to the coroutine
        // scheduler, which is not running yet. The connect happens inside the
        // slave coroutine (slave_run).
        return 0;
    }

    int TcpSlaveServer::start_eventloop()
    {
        hpc_coroutine::CoroutineSched::get_coroutine_sched()->create_coroutine(slave_run, _ip, _port);
        hpc_coroutine::CoroutineSched::get_coroutine_sched()->run();
        return 0;
    }
} // namespace hpc_coroutine