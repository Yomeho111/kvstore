#ifdef REACTOR
#include "reactor/reactor.h"
#elif defined(PROACTOR)
#include "proactor/uring_tcp.h"
#elif defined(CORROUTINE)
#include "my_coroutine/coroutine_server.h"
#endif

int main()
{
#ifdef REACTOR
    reactor::TcpServers servers(8050);
    servers.init();

    servers.start_eventloop();
#elif defined(PROACTOR)
    proactor::TcpServers servers(8050);
    servers.init();

    servers.start_eventloop();
#elif defined(CORROUTINE)
    hpc_coroutine::TcpServers servers(8050);
    servers.init();

    servers.start_eventloop();
#endif
    return 0;
}
