

#include "client.h"

#include <iostream>
#include <string>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    std::string s;
    if (argc != 3)
    {
        perror("Please provide ip port");
        return -1;
    }
    uint16_t port = atoi(argv[2]);
    kv_client::KvClient client_ins(argv[1], port);
    if (client_ins.init())
    {
        perror("error init");
        return -1;
    }
    while (1)
    {
        std::getline(std::cin, s);
        char *response = client_ins.submit_request(s);
        std::cout << response << std::endl;
        free(response);
    }
}