#include "replicate.h"
#include <iostream>

int main()
{
    replicate::SlaveServer slave("10.0.0.4", 20000);
    slave.init();

    slave.listen();

    std::cout << "ip: " << slave.get_ip() << ":" << slave.get_port() << "\n";

    slave.recv(replicate::SLAVE_TMP);

    return 0;
}