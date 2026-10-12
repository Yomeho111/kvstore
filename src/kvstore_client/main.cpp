

#include "client.h"

#include <iostream>
#include <string>
#include <vector>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "kv_header.h"

bool is_keyword(const char *command, int command_size, const char *keyword)
{
    if (command == nullptr || keyword == nullptr || command_size < 0)
        return false;

    int begin = 0;
    int end = command_size;

    while (begin < end &&
           (command[begin] == ' ' || command[begin] == '\t'))
    {
        begin++;
    }

    while (begin < end &&
           (command[end - 1] == ' ' ||
            command[end - 1] == '\t' ||
            command[end - 1] == '\r' ||
            command[end - 1] == '\n'))
    {
        end--;
    }

    const char *com = command + begin;
    int com_size = end - begin;

    size_t keyword_size = strlen(keyword); // consider the length of keyword

    return static_cast<size_t>(com_size) == keyword_size &&
           strncmp(com, keyword, keyword_size) == 0;
}

int main(int argc, char **argv)
{
    string s;
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

    bool is_batch{false};

    std::vector<kv_client::KvRequest, allocator::MyAllocator<kv_client::KvRequest>> batch;

    while (1)
    {
        std::getline(std::cin, s);
        if (is_keyword(s.c_str(), s.size(), kv_protocal::command_str[kv_protocal::KVS_EXIT]))
            return 0;

        if (!is_batch && is_keyword(s.c_str(), s.size(), kv_protocal::command_str[kv_protocal::KVS_MULTI]))
        {
            is_batch = true;
            batch.clear();
            continue;
        }

        if (is_batch)
        {
            if (is_keyword(s.c_str(), s.size(), kv_protocal::command_str[kv_protocal::KVS_EXEC]))
            {
                is_batch = false;
                if (batch.empty())
                    continue;

                kv_client::KvBatchResponse batch_response{};
                if (client_ins.submit_batch(batch.data(),
                                            static_cast<uint32_t>(batch.size()),
                                            &batch_response) != 0)
                {
                    std::cout << "ERROR" << std::endl;
                    batch.clear();
                    continue;
                }

                for (uint32_t i = 0; i < batch_response.num_response; i++)
                {
                    if (batch_response.responses[i].data != nullptr)
                        std::cout << i << ") " << batch_response.responses[i].data << std::endl;
                    else
                        std::cout << "ERROR" << std::endl;
                }

                kv_client::KvClient::free_batch_response(&batch_response);
                batch.clear();
                continue;
            }

            kv_client::KvRequest request;
            if (kv_client::parse_request_line(string(s.data(), s.size()), &request) != 0)
            {
                std::cout << "ERROR: invalid command, skipped" << std::endl;
                continue;
            }
            batch.push_back(std::move(request));
            continue;
        }
        char *response = client_ins.submit_request(s);
        if (response == nullptr)
        {
            std::cout << "ERROR" << std::endl;
            continue;
        }
        std::cout << response << std::endl;
        allocator::kv_free(response);
    }
}