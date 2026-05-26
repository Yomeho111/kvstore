#include "client.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <string>

#include "allocator.h"

#define TIME_SUB_MS(tv1, tv2) ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

using string = std::basic_string<
    char,
    std::char_traits<char>,
    allocator::MyAllocator<char>>;

void batch_testcase(kv_client::KvClient &client, const kv_client::KvRequest *requests, const char **patterns, uint32_t count, const char *casename)
{
    if (requests == nullptr || patterns == nullptr || count == 0 || casename == nullptr)
        return;

    kv_client::KvBatchResponse response{};
    if (client.submit_batch(requests, count, &response) != 0)
    {
        printf("==> FAILED -> %s, no batch response\n", casename);
        exit(1);
    }

    if (response.num_response != count)
    {
        printf("==> FAILED -> %s, response count %u != %u\n", casename, response.num_response, count);
        kv_client::KvClient::free_batch_response(&response);
        exit(1);
    }

    for (uint32_t i = 0; i < count; i++)
    {
        if (strcmp(response.responses[i].data, patterns[i]) != 0)
        {
            printf("==> FAILED -> %s[%u], '%s' != '%s' \n", casename, i, response.responses[i].data, patterns[i]);
            kv_client::KvClient::free_batch_response(&response);
            exit(1);
        }
    }

    kv_client::KvClient::free_batch_response(&response);
}

void testcase1(kv_client::KvClient &client)
{
    kv_client::KvRequest requests[] = {
        {"SET", "Teacher", "King"},
        {"GET", "Teacher", ""},
        {"MOD", "Teacher", "Darren"},
        {"GET", "Teacher", ""},
        {"EXIST", "Teacher", ""},
        {"DEL", "Teacher", ""},
        {"GET", "Teacher", ""},
        {"MOD", "Teacher", "KING"},
        {"EXIST", "Teacher", ""},
    };
    const char *patterns[] = {
        "OK\r\n",
        "King\r\n",
        "OK\r\n",
        "Darren\r\n",
        "EXIST\r\n",
        "OK\r\n",
        "NOT EXIST\r\n",
        "NOT EXIST\r\n",
        "NOT EXIST\r\n",
    };

    batch_testcase(client, requests, patterns, sizeof(requests) / sizeof(requests[0]), "batch-basic");
}

void testcase2(kv_client::KvClient &client)
{

    static char long_value[13000];
    for (int i = 0; i < sizeof(long_value) - 1; i++)
    {
        long_value[i] = 'A' + (i % 26); // A-Z pattern
    }
    long_value[sizeof(long_value) - 1] = '\0';

    // Expected GET response: "<value>\r\n"
    static char expected_get[14000];
    snprintf(expected_get, sizeof(expected_get), "%s\r\n", long_value);

    // -------- Testcases --------

    kv_client::KvRequest requests[] = {
        {"SET", "BigKey", long_value},
        {"GET", "BigKey", ""},
        {"MOD", "BigKey", long_value},
        {"GET", "BigKey", ""},
        {"EXIST", "BigKey", ""},
        {"DEL", "BigKey", ""},
        {"GET", "BigKey", ""},
        {"MOD", "BigKey", long_value},
        {"EXIST", "BigKey", ""},
    };
    const char *patterns[] = {
        "OK\r\n",
        expected_get,
        "OK\r\n",
        expected_get,
        "EXIST\r\n",
        "OK\r\n",
        "NOT EXIST\r\n",
        "NOT EXIST\r\n",
        "NOT EXIST\r\n",
    };

    batch_testcase(client, requests, patterns, sizeof(requests) / sizeof(requests[0]), "batch-large-value");
}

void array_testcase_1w(kv_client::KvClient &client, void (*func)(kv_client::KvClient &))
{

    int count = 10000;
    int i = 0;

    struct timeval tv_begin;
    gettimeofday(&tv_begin, NULL);

    for (i = 0; i < count; i++)
    {

        func(client);
    }

    struct timeval tv_end;
    gettimeofday(&tv_end, NULL);

    int time_used = TIME_SUB_MS(tv_end, tv_begin); // ms

    printf("rbtree testcase --> time_used: %d, qps: %d\n", time_used, 90000 * 1000 / time_used);
}

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        perror("Please provide ip port test_mode");
        return -1;
    }
    uint16_t port = atoi(argv[2]);
    kv_client::KvClient client_ins(argv[1], port);
    int mode = atoi(argv[3]);

    if (client_ins.init() != 0)
    {
        perror("Failed to initialize client");
        return -1;
    }
    if (mode == 0)
        testcase1(client_ins);
    else if (mode == 1)
        array_testcase_1w(client_ins, testcase1);
    else if (mode == 2)
        testcase2(client_ins);
    else if (mode == 3)
        array_testcase_1w(client_ins, testcase2);
    else
    {
        perror("Invalid testcase number");
        return -1;
    }
    return 0;
}