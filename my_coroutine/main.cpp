#include "coroutine_server.h"

int main()
{
    hpc_coroutine::TcpServers servers(8050);
    servers.init();

    servers.start_eventloop();
    return 0;
}