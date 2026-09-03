#ifdef REACTOR
#include "reactor/reactor.h"
#elif defined(PROACTOR)
#include "proactor/uring_tcp.h"
#elif defined(COROUTINE)
#include "my_coroutine/coroutine_server.h"
#endif

#include "kv_protocal.hpp"
#include "replicate.h"
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
    const char msg[] = "Close the server\n";
    (void)!write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    // _exit() is async-signal-safe, and it skips the static destructors that join
    // the replication thread while it is blocked waiting for a peer.
    _exit(sig);
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
            "  role        = standalone        standalone | master | slave\n"
            "  master_ip   = 10.0.0.4          required when role = slave\n"
            "  master_port = 20000             master's RDMA port\n"
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

    // A slave tracks updates locally too, so it can be promoted later.
    replicate::g_replicate = cfg.role != kv_config::Role::STANDALONE;

    KV_INFO("kvstore starting: config=%s port=%u role=%s persistence=%s log_level=%s",
            config_path,
            static_cast<unsigned>(cfg.port),
            kv_config::role_name(cfg.role),
            kv_config::persist_mode_name(cfg.persist_mode),
            kv_log::level_name(cfg.log_level));

    auto &prot = kv_protocal::KvStoreProtocal::instance();

    // In RDB mode a snapshot is taken on demand: send SIGUSR1 (e.g. `kill -USR1 <pid>`)
    // and a dedicated thread forks a child to dump the dataset via save().
    if (kv_persistent::g_persist_mode == kv_persistent::PersistMode::RDB)
    {
        signal(SIGCHLD, SIG_DFL); // allow waitpid() to reap the snapshot child
        signal(SIGUSR1, SIG_DFL);

        sigset_t save_set;
        sigemptyset(&save_set);
        sigaddset(&save_set, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &save_set, nullptr);

        std::thread(
            [&prot]()
            {
                sigset_t wait_set;
                sigemptyset(&wait_set);
                sigaddset(&wait_set, SIGUSR1);
                int sig = 0;
                while (sigwait(&wait_set, &sig) == 0)
                    prot.save();
            })
            .detach();
    }

    // Load the eBPF delta tracer before serving traffic. It exits the process on
    // failure and is otherwise first reached from inside the engine lock, where
    // exiting deadlocks against the replication thread waiting on that same lock.
    if (replicate::g_replicate)
        replicate::DeltaSyncObject::instance();

    if (is_slave)
    {
        auto &slave = replicate::SlaveServer::instance(cfg.master_port, cfg.master_ip.c_str());
        slave.server_loop();
    }
    else
    {
        if (replicate::g_replicate)
            replicate::MasterServer::instance().start();
#ifdef REACTOR
        reactor::TcpServers servers(cfg.port);
        servers.init();
        servers.start_eventloop();
#elif defined(PROACTOR)
        proactor::TcpServers servers(cfg.port);
        servers.init();
        servers.start_eventloop();
#elif defined(COROUTINE)
        hpc_coroutine::TcpServers servers(cfg.port);
        servers.init();
        servers.start_eventloop();
#endif
    }

    return 0;
}
