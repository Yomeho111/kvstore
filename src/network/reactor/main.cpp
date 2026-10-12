#include "reactor.h"

int main()
{
    reactor::TcpServers servers(8050);
    servers.init();

    servers.start_eventloop();
    return 0;
}