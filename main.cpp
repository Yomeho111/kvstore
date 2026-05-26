#ifdef REACTOR
#include "reactor/reactor.h"
#elif defined(PROACTOR)
#include "proactor/uring_tcp.h"
#elif defined(COROUTINE)
#include "my_coroutine/coroutine_server.h"
#endif

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>

void handler(int sig)
{
    printf("Close the server with %d\n", sig);
    exit(sig);
}

int main()
{
    for (int ii = 0; ii <= 64; ii++)
        signal(ii, SIG_IGN);
    signal(SIGTERM, handler);
    signal(SIGINT, handler);

#ifdef REACTOR
    reactor::TcpServers servers(8050);
    servers.init();

    servers.start_eventloop();
#elif defined(PROACTOR)
    proactor::TcpServers servers(8050);
    servers.init();

    servers.start_eventloop();
#elif defined(COROUTINE)
    hpc_coroutine::TcpServers servers(8050);
    servers.init();

    servers.start_eventloop();
#endif
    return 0;
}
