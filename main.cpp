#ifdef REACTOR
#include "reactor/reactor.h"
#elif defined(PROACTOR)
#include "proactor/uring_tcp.h"
#elif defined(COROUTINE)
#include "my_coroutine/coroutine_server.h"
#endif

#include "kv_protocal.hpp"
#include "rep_manager.h"

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <thread>

#define DEFAULT_PORT 8050

void handler(int sig)
{
    const char msg[] = "Close the server\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    exit(sig);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [--replicate] [--aof|--rdb]         start as master (persistence: --aof default, --rdb snapshot)\n"
            "  %s --slave <master_ip> <master_port>   start as slave syncing from master\n"
            "\n"
            "  In --rdb mode, send SIGUSR1 (kill -USR1 <pid>) to take a snapshot.\n",
            prog, prog);
}

int main(int argc, char *argv[])
{
    for (int ii = 0; ii <= 64; ii++)
        signal(ii, SIG_IGN);
    signal(SIGTERM, handler);
    signal(SIGINT, handler);

    bool is_slave = false;
    const char *master_ip = nullptr;
    uint16_t master_port = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--replicate") == 0)
        {
            replicate::g_replicate = true;
        }
        else if (strcmp(argv[i], "--rdb") == 0)
        {
            kv_persistent::g_persist_mode = kv_persistent::PersistMode::RDB;
        }
        else if (strcmp(argv[i], "--aof") == 0)
        {
            kv_persistent::g_persist_mode = kv_persistent::PersistMode::AOF;
        }
        else if (strcmp(argv[i], "--slave") == 0)
        {
            if (i + 2 >= argc)
            {
                usage(argv[0]);
                return -1;
            }
            is_slave = true;
            master_ip = argv[i + 1];
            master_port = static_cast<uint16_t>(atoi(argv[i + 2]));
            i += 2;
        }
        else
        {
            usage(argv[0]);
            return -1;
        }
    }

    auto &prot = kv_protocal::KvStoreProtocal::instance();
    (void)prot;

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

    if (is_slave)
    {
        // A slave always tracks updates locally so it can be promoted later.
        replicate::g_replicate = true;
#ifdef REACTOR
        reactor::TcpSlaveServer servers(master_port, master_ip);
        servers.init();
        servers.start_eventloop();
#elif defined(PROACTOR)
        proactor::TcpSlaveServer servers(master_port, master_ip);
        servers.init();
        servers.start_eventloop();
#elif defined(COROUTINE)
        hpc_coroutine::TcpSlaveServer servers(master_port, master_ip);
        servers.init();
        servers.start_eventloop();
#endif
    }
    else
    {
#ifdef REACTOR
        reactor::TcpServers servers(DEFAULT_PORT);
        servers.init();
        servers.start_eventloop();
#elif defined(PROACTOR)
        proactor::TcpServers servers(DEFAULT_PORT);
        servers.init();
        servers.start_eventloop();
#elif defined(COROUTINE)
        hpc_coroutine::TcpServers servers(DEFAULT_PORT);
        servers.init();
        servers.start_eventloop();
#endif
    }

    return 0;
}
