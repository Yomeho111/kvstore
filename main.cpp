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
            "  %s [--replicate]                       start as master (optionally enable replication)\n"
            "  %s --slave <master_ip> <master_port>   start as slave syncing from master\n",
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
