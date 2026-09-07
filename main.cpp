// #ifdef REACTOR
// #include "reactor/reactor.h"
// #elif defined(PROACTOR)
// #include "proactor/uring_tcp.h"
// #elif defined(COROUTINE)
// #include "my_coroutine/coroutine_server.h"
// #endif
#include "my_coroutine/coroutine_server.h"

#include "kv_protocal.hpp"
#include "kv_config.h"
#include "kv_log.h"

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <thread>

#define DEFAULT_CONFIG_PATH "kvstore.ini"

void handler(int sig)
{
    (void)sig;
    const char msg[] = "Close the server\n";
    (void)!write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    hpc_coroutine::g_shutdown = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [<config-path>]     defaults to ./" DEFAULT_CONFIG_PATH "\n"
            "  %s --config <path>     explicit form of the same thing\n"
            "\n"
            "Configuration file (INI):\n"
            "  [server]\n"
            "  port        = 8050              listening port\n"
            "  log_level   = info              error | warn | info | debug\n"
            "\n"
            "  [persistence]\n"
            "  mode        = none              none | aof | rdb\n"
            "\n"
            "  [replication]\n"
            "  role            = standalone    standalone | master | slave\n"
            "  master_ip       = 10.0.0.4      required when role = slave\n"
            "  master_port     = 8050          the master's listening port\n"
            "  slave_rdma_ip   = 10.0.0.4      required when role = slave: the\n"
            "                                  replica's RDMA-capable address\n"
            "  slave_rdma_port = 20000         the replica's RDMA port\n"
            "\n"
            "  In rdb mode, send SIGUSR1 (kill -USR1 <pid>) to take a snapshot.\n",
            prog, prog);
}

int main(int argc, char *argv[])
{
    for (int ii = 0; ii <= 64; ii++)
        signal(ii, SIG_IGN);
    signal(SIGTERM, handler);
    signal(SIGINT, handler);

    bool is_slave = false;
    const char *config_path = nullptr;

    // The config path may be given positionally or after --config, but only once.
    for (int i = 1; i < argc; i++)
    {
        const char *path = nullptr;

        if (strcmp(argv[i], "--config") == 0)
        {
            if (i + 1 >= argc)
            {
                usage(argv[0]);
                return -1;
            }
            path = argv[++i];
        }
        else if (argv[i][0] != '-')
        {
            path = argv[i];
        }

        if (!path || config_path)
        {
            usage(argv[0]);
            return -1;
        }

        config_path = path;
    }

    if (!config_path)
        config_path = DEFAULT_CONFIG_PATH;

    kv_config::Config cfg;
    if (kv_config::load(config_path, cfg) != 0)
        return -1;

    kv_log::set_level(cfg.log_level);
    kv_persistent::g_persist_mode = cfg.persist_mode;

    is_slave = cfg.role == kv_config::Role::SLAVE;

    KV_INFO("kvstore starting: config=%s port=%u role=%s persistence=%s log_level=%s",
            config_path,
            static_cast<unsigned>(cfg.port),
            kv_config::role_name(cfg.role),
            kv_config::persist_mode_name(cfg.persist_mode),
            kv_log::level_name(cfg.log_level));

    auto &prot = kv_protocal::KvStoreProtocal::instance();

    if (is_slave)
    {
        // slave server
        hpc_coroutine::TcpSlaveServer slave_server(cfg.master_port, cfg.slave_rdma_port, cfg.master_ip.c_str(), cfg.slave_rdma_ip.c_str());
        slave_server.start_eventloop();
    }
    else
    {
        // #ifdef REACTOR
        //         reactor::TcpServers servers(cfg.port);
        //         servers.init();
        //         servers.start_eventloop();
        // #elif defined(PROACTOR)
        //         proactor::TcpServers servers(cfg.port);
        //         servers.init();
        //         servers.start_eventloop();
        // #elif defined(COROUTINE)
        //         hpc_coroutine::TcpServers servers(cfg.port);
        //         servers.init();
        //         servers.start_eventloop();
        // #endif
        hpc_coroutine::TcpServers servers(cfg.port);
        servers.init();
        servers.start_eventloop();
    }

    return 0;
}
