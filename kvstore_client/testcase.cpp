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

void testcase(kv_client::KvClient &client, const string &command, const string &key, const string &value, const char *pattern, const char *casename)
{
    if (command.size() == 0 || key.size() == 0 || !pattern)
        return;
    char *response = client.submit_request(command, key, value);
    if (response == nullptr)
    {
        printf("==> FAILED -> %s, no response for command '%s'\n", casename, command.c_str());
        exit(1);
    }

    if (strcmp(response, pattern) == 0)
    {
        // printf("==> PASS -> %s\n", casename);
        allocator::kv_free(response);
    }
    else
    {
        printf("==> FAILED -> %s, '%s' != '%s' \n", casename, response, pattern);
        allocator::kv_free(response);
        exit(1);
    }
}

void testcase1(kv_client::KvClient &client)
{

    testcase(client, "SET", "Teacher", "King", "OK\r\n", "SET-Teacher");
    testcase(client, "GET", "Teacher", "", "King\r\n", "GET-Teacher");
    testcase(client, "MOD", "Teacher", "Darren", "OK\r\n", "MOD-Teacher");
    testcase(client, "GET", "Teacher", "", "Darren\r\n", "GET-Teacher");
    testcase(client, "EXIST", "Teacher", "", "EXIST\r\n", "GET-Teacher");
    testcase(client, "DEL", "Teacher", "", "OK\r\n", "DEL-Teacher");
    testcase(client, "GET", "Teacher", "", "NOT EXIST\r\n", "GET-Teacher");
    testcase(client, "MOD", "Teacher", "KING", "NOT EXIST\r\n", "MOD-Teacher");
    testcase(client, "EXIST", "Teacher", "", "NOT EXIST\r\n", "GET-Teacher");
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

    testcase(client, "SET", "BigKey", long_value, "OK\r\n", "SET-BigKey");
    testcase(client, "GET", "BigKey", "", expected_get, "GET-BigKey");

    testcase(client, "MOD", "BigKey", long_value, "OK\r\n", "MOD-BigKey");
    testcase(client, "GET", "BigKey", "", expected_get, "GET-BigKey");

    testcase(client, "EXIST", "BigKey", "", "EXIST\r\n", "EXIST-BigKey");

    testcase(client, "DEL", "BigKey", "", "OK\r\n", "DEL-BigKey");

    testcase(client, "GET", "BigKey", "", "NOT EXIST\r\n", "GET-BigKey");
    testcase(client, "MOD", "BigKey", long_value, "NOT EXIST\r\n", "MOD-BigKey");

    testcase(client, "EXIST", "BigKey", "", "NOT EXIST\r\n", "EXIST-BigKey");
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