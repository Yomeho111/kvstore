#include "client.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#define TIME_SUB_MS(tv1, tv2) ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

void testcase(kv_client::KvClient &client, const char *command, const char *pattern, const char *casename)
{
    if (!command || !pattern)
        return;
    char *response = client.submit_request(command);

    if (strcmp(response, pattern) == 0)
    {
        // printf("==> PASS -> %s\n", casename);
        free(response);
    }
    else
    {
        printf("==> FAILED -> %s, '%s' != '%s' \n", casename, response, pattern);
        free(response);
        exit(1);
    }
}

void testcase1(kv_client::KvClient &client)
{

    testcase(client, "SET Teacher King", "OK\r\n", "SET-Teacher");
    testcase(client, "GET Teacher", "King\r\n", "GET-Teacher");
    testcase(client, "MOD Teacher Darren", "OK\r\n", "MOD-Teacher");
    testcase(client, "GET Teacher", "Darren\r\n", "GET-Teacher");
    testcase(client, "EXIST Teacher", "EXIST\r\n", "GET-Teacher");
    testcase(client, "DEL Teacher", "OK\r\n", "DEL-Teacher");
    testcase(client, "GET Teacher", "NOT EXIST\r\n", "GET-Teacher");
    testcase(client, "MOD Teacher KING", "NOT EXIST\r\n", "MOD-Teacher");
    testcase(client, "EXIST Teacher", "NOT EXIST\r\n", "GET-Teacher");
}

void array_testcase_1w(kv_client::KvClient &client)
{

    int count = 10000;
    int i = 0;

    struct timeval tv_begin;
    gettimeofday(&tv_begin, NULL);

    for (i = 0; i < count; i++)
    {

        testcase1(client);
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
        perror("Please provide ip port");
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
        array_testcase_1w(client_ins);
    return 0;
}