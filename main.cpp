#ifdef REACTOR
#include "reactor/reactor.h"
#elif defined(PROACTOR)
#include "proactor/uring_tcp.h"
#elif defined(COROUTINE)
#include "my_coroutine/coroutine_server.h"
#endif

#include "kv_protocal.hpp"
#include "replicate.h"

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
    (void)!write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    // _exit() is async-signal-safe, and it skips the static destructors that join
    // the replication thread while it is blocked waiting for a peer.
    _exit(sig);
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

    // A slave always tracks updates locally so it can be promoted later.
    if (is_slave)
        replicate::g_replicate = true;

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
        auto &slave = replicate::SlaveServer::instance(master_port, master_ip);
        slave.server_loop();
    }
    else
    {
        if (replicate::g_replicate)
            replicate::MasterServer::instance().start();
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
