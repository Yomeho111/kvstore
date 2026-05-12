#include "uring_tcp.h"

int main()
{
    proactor::TcpServers servers(8050);
    servers.init();

    servers.start_eventloop();
    return 0;
}