#include "coroutine_server.h"
#include "allocator.h"
#include "status.h"
#include "kv_log.h"

#include <errno.h>
#include <sys/uio.h>
#include <string>
#include <thread>
#include "hiredis.h"
#include "kv_log.h"

#define RESP_RECV_BUF_SIZE 32768
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
                KV_ERROR("init_server");
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
            KV_ERROR("error socket");
            return -1;
        }

        struct sockaddr_in local, remote;
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        local.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&local, sizeof(struct sockaddr_in)) == -1)
        {
            KV_ERROR("error bind");
            close(fd);
            return -1;
        }

        if (listen(fd, 20) == -1)
        {
            KV_ERROR("error listen");
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

        char *buf = (char *)allocator::kv_malloc(RESP_RECV_BUF_SIZE);
        if (!buf)
        {
            close(fd);
            return;
        }
        while (1)
        {
            int n = recv(fd, buf, RESP_RECV_BUF_SIZE, 0);
            if (n <= 0)
                break;

            if (redisReaderFeed(reader, buf, n) != REDIS_OK)
                break;

            string out;
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
                        const uint64_t cmd = kv_protocal::cmd_tag(argv[0], argvlen[0]);
                        if (kv_protocal::KvStoreProtocal::instance().process_resp_command(argc, argv, argvlen, cmd, out) == 1)
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
        allocator::kv_free(buf);
    }

    void resp_slave_process(int fd)
    {
        redisReader *reader = redisReaderCreate();
        if (reader == nullptr)
            return;

        char *buf = (char *)allocator::kv_malloc(RESP_RECV_BUF_SIZE);
        if (!buf)
        {
            redisReaderFree(reader);
            return;
        }
        while (1)
        {
            int n = recv(fd, buf, RESP_RECV_BUF_SIZE, 0);
            if (n <= 0)
                break;

            if (redisReaderFeed(reader, buf, n) != REDIS_OK)
                break;

            void *reply = nullptr;

            // A single read may carry several pipelined commands; drain them all.
            while (redisReaderGetReply(reader, &reply) == REDIS_OK && reply != nullptr)
            {
                redisReply *rr = static_cast<redisReply *>(reply);
                if (rr->type == REDIS_REPLY_ARRAY && rr->elements > 0)
                {
                    int argc = static_cast<int>(rr->elements);
                    if (argc <= RESP_MAX_ARGS)
                    {
                        char *argv[RESP_MAX_ARGS];
                        size_t argvlen[RESP_MAX_ARGS];
                        for (int i = 0; i < argc; i++)
                        {
                            argv[i] = rr->element[i]->str;
                            argvlen[i] = rr->element[i]->len;
                        }
                        const uint64_t cmd = kv_protocal::cmd_tag(argv[0], argvlen[0]);
                        if (cmd == kv_protocal::KV_SET || cmd == kv_protocal::KV_DEL || cmd == kv_protocal::KV_PEXPIRE || cmd == kv_protocal::KV_SETEX || cmd == kv_protocal::KV_PSETEX || cmd == kv_protocal::KV_EXPIRE)
                            kv_protocal::KvStoreProtocal::instance().process_resp_command(argc, argv, argvlen, cmd, kv_protocal::RespSink{});
                    }
                }

                freeReplyObject(reply);
                reply = nullptr;
            }

            if (reader->err)
                break;
        }

        redisReaderFree(reader);
        allocator::kv_free(buf);
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
                    KV_ERROR("error recv");
                close(fd);
                return;
            }

            if (num_header.num_request == 0)
                continue;

            if (kv_protocal::KvStoreProtocal::instance().process_num_request(&status, num_header.num_request) != 0)
            {
                KV_ERROR("Processing number of request failure");
                should_close = true;
                goto clean;
            }

            header_size = status.num_request * kv_protocal::HEADER_SIZE;
            status.req_info = (kv_protocal::HeaderInfo *)allocator::kv_malloc(header_size);
            if (status.req_info == nullptr)
            {
                KV_ERROR("Error Memory allocation");
                goto clean;
            }

            ret = recv(fd, status.req_info, header_size, 0);
            if (ret <= 0)
            {
                if (ret < 0)
                    KV_ERROR("error recv");
                should_close = true;
                goto clean;
            }

            if (kv_protocal::KvStoreProtocal::instance().process_header(&status, &r_iovec) != 0)
            {
                KV_ERROR("Processing header failure");
                should_close = true;
                goto clean;
            }

            ret = readv_full(fd, r_iovec, status.num_request);
            if (ret <= 0)
            {
                if (ret < 0)
                    KV_ERROR("error read");
                should_close = true;
                goto clean;
            }

            process_count = kv_protocal::KvStoreProtocal::instance().process_body(&status, r_iovec, &w_iovec);
            if (process_count < 0)
            {
                KV_ERROR("Error handling body");
                should_close = true;
                goto clean;
            }

            if (status.status == network::SEND_RESPONSE)
            {
                ret = writev_all(fd, w_iovec, status.w_iovec_size);
                if (ret <= 0)
                {
                    if (ret < 0)
                        KV_ERROR("Error write");
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
            hpc_coroutine::CoroutineSched::get_coroutine_sched()->create_coroutine(resp_server_process, cli_fd);
        }
    }

    void slave_process(int fd, uint16_t port_rdma, const char *ip_rdma)
    {
        if (!ip_rdma || fd < 0)
            return;

        char buf[32]{0};
        int n{0};
        int ret{0};

        // launch the slave server
        replicate::SlaveServer slave(ip_rdma, port_rdma);
        if (slave.init() < 0)
        {
            KV_ERROR("slave launched error");
            close(fd);
            return;
        }
        KV_INFO("slave rdma init succeed on %s: %u", ip_rdma, port_rdma);

        // use sub-thread to receive full sync data
        std::thread thr{[&slave, &ret]
                        {ret = slave.listen(); if (ret == 0) ret = slave.recv(replicate::SLAVE_TMP); }};

        // make command for sync
        const char *command = kv_protocal::command_str[kv_protocal::KVS_SYNC];
        char port_str[32]{0};
        snprintf(port_str, sizeof(port_str), "%u", port_rdma);

        auto &prot = kv_protocal::KvStoreProtocal::instance();
        auto rdma_payload = prot.make_command(command, strnlen(command, 32), ip_rdma, port_str);
        KV_INFO("SYNC command: %s", rdma_payload.data);
        if (rdma_payload.data == nullptr || rdma_payload.size == 0)
        {
            KV_ERROR("SYNC command error");
            goto clean;
        }

        if (send(fd, rdma_payload.data, rdma_payload.size, 0) < 0)
        {
            KV_ERROR("SYNC send error");
            goto clean;
        }

        KV_INFO("SYNC command sent");

        n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            KV_ERROR("SYNC recv error");
            goto clean;
        }

        KV_INFO("SYNC response get: %s", buf);

        if (strcmp(buf, "+OK\r\n"))
        {
            KV_ERROR("SYNC failed signal from master");
            goto clean;
        }

        // remove rdma resource
        if (thr.joinable())
            thr.join();

        // load data
        if (prot.load_snapshot(replicate::SLAVE_TMP, true) < 0)
        {
            KV_ERROR("SYNC data load error");
            goto clean;
        }

        KV_INFO("Load temp data");

        // remove tmp data
        if (access(replicate::SLAVE_TMP, F_OK) == 0 && remove(replicate::SLAVE_TMP) != 0)
        {
            KV_ERROR("tmp file remove error");
            goto clean;
        }

        KV_INFO("remove temp data");

        if (send(fd, kv_protocal::SYNCFIN_RESP, strnlen(kv_protocal::SYNCFIN_RESP, 32), 0) < 0)
        {
            KV_ERROR("SYNC send error");
            goto clean;
        }

    clean:
        if (thr.joinable())
            thr.join();
        if (rdma_payload.data)
            allocator::kv_free(rdma_payload.data);
        close(fd);
        KV_INFO("rdma resource clean");
        return;
    }

    // Runs inside a coroutine: the hooked socket()/connect()/recv()/send()
    // yield to the scheduler, so they must NOT be called before the scheduler
    // is running (i.e. not from TcpSlaveServer::init()).
    void slave_run(uint16_t port, uint16_t port_rdma, const char *ip, const char *ip_rdma)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            KV_ERROR("socket");
            return;
        }

        struct sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);

        if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0)
        {
            KV_ERROR("inet_pton");
            close(fd);
            return;
        }

        if (connect(fd, reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr)) < 0)
        {
            KV_ERROR("connect");
            close(fd);
            return;
        }

        slave_process(fd, port_rdma, ip_rdma);
    }

    int TcpSlaveServer::start_eventloop()
    {
        hpc_coroutine::CoroutineSched::get_coroutine_sched()->create_coroutine(slave_run, _master_port, _port_rdma, _master_ip, _ip_rdma);
        hpc_coroutine::CoroutineSched::get_coroutine_sched()->run();
        return 0;
    }
} // namespace hpc_coroutine