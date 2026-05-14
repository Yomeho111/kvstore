#include "coroutine_server.h"

namespace hpc_coroutine
{
    void server(void *arg);

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
                hpc_coroutine::CoroutineSched::get_coroutine_sched()->create_coroutine(server, &_fd_list[i]);
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

    void server_process(void *arg)
    {
        int fd = *(int *)arg;
        free(arg);
        int ret = 0;
        struct network::StatusM status_m;

        while (1)
        {
            char buf[1024] = {0};
            ret = recv(fd, buf, 1024, 0);
            if (ret > 0)
            {
                ret = send(fd, buf, strlen(buf), 0);
                if (ret == -1)
                {
                    close(fd);
                    break;
                }
            }
            else if (ret == 0)
            {
                close(fd);
                break;
            }
        }
    }

    void server(void *arg)
    {
        int fd = *(int *)arg;

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

            int *arg = (int *)malloc(sizeof(int));
            *arg = cli_fd;
            hpc_coroutine::CoroutineSched::get_coroutine_sched()->create_coroutine(server_process, arg);
        }
    }
}