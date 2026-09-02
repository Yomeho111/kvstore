#include "coroutine_server.h"
#include "allocator.h"
#include "status.h"

#include <errno.h>
#include <sys/uio.h>
#include <string>
#include "hiredis.h"

#define HEARTBEAT_INTERVAL_MS 2000
#define RESP_RECV_BUF_SIZE 16384
#define RESP_MAX_ARGS 64

namespace hpc_coroutine
{
    void server(int fd);
    void resp_server_process(int fd);

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

    // Handle a RESP (Redis protocol) client connection. Mirrors the reactor's
    // resp_recv_callback but in the coroutine's synchronous style: the hooked
    // recv()/send() yield the coroutine while waiting on the socket.
    void resp_server_process(int fd)
    {
        redisReader *reader = redisReaderCreate();
        if (reader == nullptr)
        {
            close(fd);
            return;
        }

        char buf[RESP_RECV_BUF_SIZE];
        while (1)
        {
            int n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
                break;

            if (redisReaderFeed(reader, buf, n) != REDIS_OK)
                break;

            std::string out;
            bool close_after = false;
            void *reply = nullptr;

            // A single read may carry several pipelined commands; drain them all.
            while (redisReaderGetReply(reader, &reply) == REDIS_OK && reply != nullptr)
            {
                redisReply *rr = static_cast<redisReply *>(reply);
                if (rr->type == REDIS_REPLY_ARRAY && rr->elements > 0)
                {
                    int argc = static_cast<int>(rr->elements);
                    if (argc > RESP_MAX_ARGS)
                    {
                        out += "-ERR too many arguments\r\n";
                    }
                    else
                    {
                        char *argv[RESP_MAX_ARGS];
                        size_t argvlen[RESP_MAX_ARGS];
                        for (int i = 0; i < argc; i++)
                        {
                            argv[i] = rr->element[i]->str;
                            argvlen[i] = rr->element[i]->len;
                        }
                        if (kv_protocal::KvStoreProtocal::instance().process_resp_command(argc, argv, argvlen, out) == 1)
                            close_after = true;
                    }
                }
                else
                {
                    out += "-ERR protocol error\r\n";
                }
                freeReplyObject(reply);
                reply = nullptr;
            }

            if (reader->err)
            {
                out += "-ERR protocol error\r\n";
                close_after = true;
            }

            if (!out.empty())
            {
                if (send(fd, out.data(), out.size(), 0) < 0)
                    break;
            }

            if (close_after)
                break;
        }

        redisReaderFree(reader);
        close(fd);
    }

    void server_process(int fd)
    {
        // Detect the wire protocol from the first byte: RESP multi-bulk commands
        // (redis-cli / hiredis / redis-benchmark) always begin with '*'.
        char peek_c = 0;
        int pk = recv(fd, &peek_c, 1, MSG_PEEK);
        if (pk <= 0)
        {
            close(fd);
            return;
        }
        if (peek_c == '*')
        {
            resp_server_process(fd);
            return;
        }

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
} // namespace hpc_coroutine