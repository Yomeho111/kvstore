#include "client.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <string>
#include <vector>

#include "allocator.h"
#include "hiredis.h"

#define N 500000

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

void testcase_timeout(kv_client::KvClient &client)
{
    kv_protocal::TimeoutSpec set_timeout{0, 200 * 1000 * 1000};
    kv_protocal::TimeoutSpec mod_timeout{0, 300 * 1000 * 1000};

    kv_client::KvRequest cleanup_requests[] = {
        {"DEL", "TimeoutSet", ""},
        {"DEL", "TimeoutMod", ""},
    };

    kv_client::KvBatchResponse cleanup_response{};
    if (client.submit_batch(cleanup_requests, sizeof(cleanup_requests) / sizeof(cleanup_requests[0]), &cleanup_response) == 0)
        kv_client::KvClient::free_batch_response(&cleanup_response);

    kv_client::KvRequest set_mod_requests[] = {
        {"SET", "TimeoutSet", "Alpha", set_timeout},
        {"GET", "TimeoutSet", ""},
        {"SET", "TimeoutMod", "Before"},
        {"MOD", "TimeoutMod", "After", mod_timeout},
        {"GET", "TimeoutMod", ""},
        {"EXIST", "TimeoutSet", ""},
        {"EXIST", "TimeoutMod", ""},
    };

    const char *set_mod_patterns[] = {
        "OK\r\n",
        "Alpha\r\n",
        "OK\r\n",
        "OK\r\n",
        "After\r\n",
        "EXIST\r\n",
        "EXIST\r\n",
    };

    batch_testcase(client, set_mod_requests, set_mod_patterns, sizeof(set_mod_requests) / sizeof(set_mod_requests[0]), "batch-timeout-before-expire");

    usleep(600 * 1000);

    kv_client::KvRequest expired_requests[] = {
        {"GET", "TimeoutSet", ""},
        {"EXIST", "TimeoutSet", ""},
        {"GET", "TimeoutMod", ""},
        {"EXIST", "TimeoutMod", ""},
    };

    const char *expired_patterns[] = {
        "NOT EXIST\r\n",
        "NOT EXIST\r\n",
        "NOT EXIST\r\n",
        "NOT EXIST\r\n",
    };

    batch_testcase(client, expired_requests, expired_patterns, sizeof(expired_requests) / sizeof(expired_requests[0]), "batch-timeout-after-expire");
}

void testcase_set(kv_client::KvClient &client)
{
    constexpr int BATCH_SIZE = 128;

    const char *value =
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";

    for (int begin = 1; begin <= N; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE - 1, N);
        int batch_count = end - begin + 1;

        std::vector<std::string> keys;
        std::vector<kv_client::KvRequest> requests;
        std::vector<const char *> patterns;

        keys.reserve(batch_count);
        requests.reserve(batch_count);
        patterns.reserve(batch_count);

        for (int i = begin; i <= end; ++i)
        {
            keys.emplace_back("key" + std::to_string(i));

            requests.push_back({"SET",
                                keys.back().c_str(),
                                value});

            patterns.push_back("OK\r\n");
        }

        std::string testcase_name =
            "batch-set-10000-" + std::to_string(begin) + "-" + std::to_string(end);

        batch_testcase(
            client,
            requests.data(),
            patterns.data(),
            requests.size(),
            testcase_name.c_str());
    }
}

void testcase_del(kv_client::KvClient &client)
{
    constexpr int BATCH_SIZE = 128;

    for (int begin = 1; begin <= N; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE - 1, N);
        int batch_count = end - begin + 1;

        std::vector<std::string> keys;
        std::vector<kv_client::KvRequest> requests;
        std::vector<const char *> patterns;

        keys.reserve(batch_count);
        requests.reserve(batch_count);
        patterns.reserve(batch_count);

        for (int i = begin; i <= end; ++i)
        {
            keys.emplace_back("key" + std::to_string(i));

            requests.push_back({"DEL",
                                keys.back().c_str(),
                                ""});

            patterns.push_back("OK\r\n");
        }

        std::string testcase_name =
            "batch-del-10000-" + std::to_string(begin) + "-" + std::to_string(end);

        batch_testcase(
            client,
            requests.data(),
            patterns.data(),
            requests.size(),
            testcase_name.c_str());
    }
}

constexpr int UNIQUE_KV_COUNT = 100000;

static std::string make_unique_key(int i)
{
    return "ukey" + std::to_string(i);
}

static std::string make_unique_value(int i)
{
    return "uval" + std::to_string(i);
}

// SET UNIQUE_KV_COUNT key/value pairs, each with a distinct key and a distinct value.
void testcase_set_unique(kv_client::KvClient &client)
{
    constexpr int BATCH_SIZE = 128;

    for (int begin = 1; begin <= UNIQUE_KV_COUNT; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE - 1, UNIQUE_KV_COUNT);
        int batch_count = end - begin + 1;

        std::vector<std::string> keys;
        std::vector<std::string> values;
        std::vector<kv_client::KvRequest> requests;
        std::vector<const char *> patterns;

        keys.reserve(batch_count);
        values.reserve(batch_count);
        requests.reserve(batch_count);
        patterns.reserve(batch_count);

        for (int i = begin; i <= end; ++i)
        {
            keys.emplace_back(make_unique_key(i));
            values.emplace_back(make_unique_value(i));

            requests.push_back({"SET",
                                keys.back().c_str(),
                                values.back().c_str()});

            patterns.push_back("OK\r\n");
        }

        std::string testcase_name =
            "batch-set-unique-" + std::to_string(begin) + "-" + std::to_string(end);

        batch_testcase(
            client,
            requests.data(),
            patterns.data(),
            requests.size(),
            testcase_name.c_str());
    }
}

// First half of testcase_set_unique: SET ukey1 .. ukey[UNIQUE_KV_COUNT/2].
void testcase_set_unique_first_half(kv_client::KvClient &client)
{
    constexpr int BATCH_SIZE = 128;
    constexpr int HALF = UNIQUE_KV_COUNT / 2;

    for (int begin = 1; begin <= HALF; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE - 1, HALF);
        int batch_count = end - begin + 1;

        std::vector<std::string> keys;
        std::vector<std::string> values;
        std::vector<kv_client::KvRequest> requests;
        std::vector<const char *> patterns;

        keys.reserve(batch_count);
        values.reserve(batch_count);
        requests.reserve(batch_count);
        patterns.reserve(batch_count);

        for (int i = begin; i <= end; ++i)
        {
            keys.emplace_back(make_unique_key(i));
            values.emplace_back(make_unique_value(i));

            requests.push_back({"SET",
                                keys.back().c_str(),
                                values.back().c_str()});

            patterns.push_back("OK\r\n");
        }

        std::string testcase_name =
            "batch-set-unique-first-" + std::to_string(begin) + "-" + std::to_string(end);

        batch_testcase(
            client,
            requests.data(),
            patterns.data(),
            requests.size(),
            testcase_name.c_str());
    }
}

// Second half of testcase_set_unique: SET ukey[UNIQUE_KV_COUNT/2 + 1] .. ukey[UNIQUE_KV_COUNT].
void testcase_set_unique_second_half(kv_client::KvClient &client)
{
    constexpr int BATCH_SIZE = 128;
    constexpr int HALF = UNIQUE_KV_COUNT / 2;

    for (int begin = HALF + 1; begin <= UNIQUE_KV_COUNT; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE - 1, UNIQUE_KV_COUNT);
        int batch_count = end - begin + 1;

        std::vector<std::string> keys;
        std::vector<std::string> values;
        std::vector<kv_client::KvRequest> requests;
        std::vector<const char *> patterns;

        keys.reserve(batch_count);
        values.reserve(batch_count);
        requests.reserve(batch_count);
        patterns.reserve(batch_count);

        for (int i = begin; i <= end; ++i)
        {
            keys.emplace_back(make_unique_key(i));
            values.emplace_back(make_unique_value(i));

            requests.push_back({"SET",
                                keys.back().c_str(),
                                values.back().c_str()});

            patterns.push_back("OK\r\n");
        }

        std::string testcase_name =
            "batch-set-unique-second-" + std::to_string(begin) + "-" + std::to_string(end);

        batch_testcase(
            client,
            requests.data(),
            patterns.data(),
            requests.size(),
            testcase_name.c_str());
    }
}

// GET the UNIQUE_KV_COUNT pairs written by testcase_set_unique and verify each value.
void testcase_get_unique(kv_client::KvClient &client)
{
    constexpr int BATCH_SIZE = 128;

    for (int begin = 1; begin <= UNIQUE_KV_COUNT; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE - 1, UNIQUE_KV_COUNT);
        int batch_count = end - begin + 1;

        std::vector<std::string> keys;
        std::vector<std::string> expected;
        std::vector<kv_client::KvRequest> requests;
        std::vector<const char *> patterns;

        keys.reserve(batch_count);
        expected.reserve(batch_count);
        requests.reserve(batch_count);
        patterns.reserve(batch_count);

        for (int i = begin; i <= end; ++i)
        {
            keys.emplace_back(make_unique_key(i));
            expected.emplace_back(make_unique_value(i) + "\r\n");

            requests.push_back({"GET",
                                keys.back().c_str(),
                                ""});

            patterns.push_back(expected.back().c_str());
        }

        std::string testcase_name =
            "batch-get-unique-" + std::to_string(begin) + "-" + std::to_string(end);

        batch_testcase(
            client,
            requests.data(),
            patterns.data(),
            requests.size(),
            testcase_name.c_str());
    }
}

// ---------------------------------------------------------------------------
// Multi-step timer / expiration test.
//
//   t0        : SET TIMER_STEP_KV unique KV, each with a 1s expiration.
//   T1 .. T5  : 1.5s after the previous step, GET the previous step's batch
//               (every key must have expired -> "NOT EXIST") and then SET a
//               fresh batch of TIMER_STEP_KV unique KV, again with a 1s TTL.
//
// The 1.5s spacing is deliberately larger than the 1s TTL, so each batch is
// guaranteed to be gone by the time it is read back one step later. This keeps
// the server-side timer under a sustained set/expire workload.
// ---------------------------------------------------------------------------
constexpr int TIMER_STEP_KV = 10000;                // unique KV per step
constexpr int TIMER_STEPS = 5;                      // T1 .. T5
constexpr int TIMER_STEP_INTERVAL_US = 1500 * 1000; // 1.5s between steps

static std::string make_timer_key(int step, int i)
{
    return "tmkey_" + std::to_string(step) + "_" + std::to_string(i);
}

static std::string make_timer_value(int step, int i)
{
    return "tmval_" + std::to_string(step) + "_" + std::to_string(i);
}

// SET the whole batch for `step`, each key carrying a 1s expiration.
static void timer_set_batch(kv_client::KvClient &client, int step)
{
    constexpr int BATCH_SIZE = 128;
    const kv_protocal::TimeoutSpec ttl{1, 0}; // 1 second

    for (int begin = 0; begin < TIMER_STEP_KV; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE, TIMER_STEP_KV);
        int batch_count = end - begin;

        std::vector<std::string> keys;
        std::vector<std::string> values;
        std::vector<kv_client::KvRequest> requests;
        std::vector<const char *> patterns;

        keys.reserve(batch_count);
        values.reserve(batch_count);
        requests.reserve(batch_count);
        patterns.reserve(batch_count);

        for (int i = begin; i < end; ++i)
        {
            keys.emplace_back(make_timer_key(step, i));
            values.emplace_back(make_timer_value(step, i));

            requests.push_back({"SET",
                                keys.back().c_str(),
                                values.back().c_str(),
                                ttl});
            patterns.push_back("OK\r\n");
        }

        std::string testcase_name =
            "timer-set-step" + std::to_string(step) + "-" + std::to_string(begin);

        batch_testcase(
            client,
            requests.data(),
            patterns.data(),
            requests.size(),
            testcase_name.c_str());
    }
}

// GET the whole batch for `step` and assert every key has expired.
static void timer_verify_expired(kv_client::KvClient &client, int step)
{
    constexpr int BATCH_SIZE = 128;

    for (int begin = 0; begin < TIMER_STEP_KV; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE, TIMER_STEP_KV);
        int batch_count = end - begin;

        std::vector<std::string> keys;
        std::vector<kv_client::KvRequest> requests;
        std::vector<const char *> patterns;

        keys.reserve(batch_count);
        requests.reserve(batch_count);
        patterns.reserve(batch_count);

        for (int i = begin; i < end; ++i)
        {
            keys.emplace_back(make_timer_key(step, i));

            requests.push_back({"GET",
                                keys.back().c_str(),
                                ""});
            patterns.push_back("NOT EXIST\r\n");
        }

        std::string testcase_name =
            "timer-expired-step" + std::to_string(step) + "-" + std::to_string(begin);

        batch_testcase(
            client,
            requests.data(),
            patterns.data(),
            requests.size(),
            testcase_name.c_str());
    }
}

void testcase_timer_multi_step(kv_client::KvClient &client)
{
    // t0: seed the first batch.
    timer_set_batch(client, 0);
    printf("timer-multi-step: t0 SET %d KV (1s TTL)\n", TIMER_STEP_KV);

    // T1 .. T5: wait past the TTL, confirm the previous batch expired, seed the next.
    for (int step = 1; step <= TIMER_STEPS; ++step)
    {
        usleep(TIMER_STEP_INTERVAL_US);

        timer_verify_expired(client, step - 1);
        timer_set_batch(client, step);

        printf("timer-multi-step: T%d verified step %d expired + SET %d new KV\n",
               step, step - 1, TIMER_STEP_KV);
    }

    printf("==> PASSED -> timer-multi-step (%d steps, %d KV/step, 1s TTL, 1.5s spacing)\n",
           TIMER_STEPS, TIMER_STEP_KV);
}

// ===========================================================================
// RESP protocol testcases driven through the hiredis synchronous client.
// These mirror testcase_set_unique / testcase_get_unique / testcase_timer_multi_step
// but talk to the server over Redis RESP instead of the native KvClient.
// ===========================================================================

static redisContext *resp_connect(const char *ip, uint16_t port)
{
    redisContext *c = redisConnect(ip, port);
    if (c == nullptr || c->err)
    {
        printf("==> FAILED -> resp connect: %s\n", c ? c->errstr : "cannot allocate context");
        if (c)
            redisFree(c);
        exit(1);
    }
    return c;
}

// RESP mirror of testcase_set_unique: SET every unique pair, expect +OK.
void resp_testcase_set_unique(const char *ip, uint16_t port)
{
    constexpr int BATCH_SIZE = 128;
    redisContext *c = resp_connect(ip, port);

    for (int begin = 1; begin <= UNIQUE_KV_COUNT; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE - 1, UNIQUE_KV_COUNT);

        // Pipeline the whole batch, then read the replies back.
        for (int i = begin; i <= end; ++i)
        {
            std::string key = make_unique_key(i);
            std::string value = make_unique_value(i);
            redisAppendCommand(c, "SET %s %s", key.c_str(), value.c_str());
        }

        for (int i = begin; i <= end; ++i)
        {
            redisReply *reply = nullptr;
            if (redisGetReply(c, (void **)&reply) != REDIS_OK || reply == nullptr)
            {
                printf("==> FAILED -> resp-set-unique[%d], no reply: %s\n", i, c->errstr);
                redisFree(c);
                exit(1);
            }
            if (reply->type != REDIS_REPLY_STATUS || strcmp(reply->str, "OK") != 0)
            {
                printf("==> FAILED -> resp-set-unique[%d], unexpected reply (type=%d)\n", i, reply->type);
                freeReplyObject(reply);
                redisFree(c);
                exit(1);
            }
            freeReplyObject(reply);
        }
    }

    redisFree(c);
    printf("==> PASSED -> resp-set-unique (%d KV over RESP)\n", UNIQUE_KV_COUNT);
}

// RESP mirror of testcase_get_unique: GET every pair, verify the bulk value.
void resp_testcase_get_unique(const char *ip, uint16_t port)
{
    constexpr int BATCH_SIZE = 128;
    redisContext *c = resp_connect(ip, port);

    for (int begin = 1; begin <= UNIQUE_KV_COUNT; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE - 1, UNIQUE_KV_COUNT);

        for (int i = begin; i <= end; ++i)
        {
            std::string key = make_unique_key(i);
            redisAppendCommand(c, "GET %s", key.c_str());
        }

        for (int i = begin; i <= end; ++i)
        {
            std::string expected = make_unique_value(i);
            redisReply *reply = nullptr;
            if (redisGetReply(c, (void **)&reply) != REDIS_OK || reply == nullptr)
            {
                printf("==> FAILED -> resp-get-unique[%d], no reply: %s\n", i, c->errstr);
                redisFree(c);
                exit(1);
            }
            // RESP GET returns a bulk string with the raw value (no trailing CRLF).
            if (reply->type != REDIS_REPLY_STRING ||
                reply->len != expected.size() ||
                memcmp(reply->str, expected.c_str(), reply->len) != 0)
            {
                printf("==> FAILED -> resp-get-unique[%d], unexpected reply (type=%d)\n", i, reply->type);
                freeReplyObject(reply);
                redisFree(c);
                exit(1);
            }
            freeReplyObject(reply);
        }
    }

    redisFree(c);
    printf("==> PASSED -> resp-get-unique (%d KV over RESP)\n", UNIQUE_KV_COUNT);
}

// SET the whole batch for `step` over RESP, each key carrying a 1s expiry (EX 1).
static void resp_timer_set_batch(redisContext *c, int step)
{
    constexpr int BATCH_SIZE = 128;

    for (int begin = 0; begin < TIMER_STEP_KV; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE, TIMER_STEP_KV);

        for (int i = begin; i < end; ++i)
        {
            std::string key = make_timer_key(step, i);
            std::string value = make_timer_value(step, i);
            redisAppendCommand(c, "SET %s %s EX 1", key.c_str(), value.c_str());
        }

        for (int i = begin; i < end; ++i)
        {
            redisReply *reply = nullptr;
            if (redisGetReply(c, (void **)&reply) != REDIS_OK || reply == nullptr)
            {
                printf("==> FAILED -> resp-timer-set step%d[%d], no reply\n", step, i);
                redisFree(c);
                exit(1);
            }
            if (reply->type != REDIS_REPLY_STATUS || strcmp(reply->str, "OK") != 0)
            {
                printf("==> FAILED -> resp-timer-set step%d[%d], unexpected reply (type=%d)\n", step, i, reply->type);
                freeReplyObject(reply);
                redisFree(c);
                exit(1);
            }
            freeReplyObject(reply);
        }
    }
}

// GET the whole batch for `step` over RESP and assert every key has expired (nil).
static void resp_timer_verify_expired(redisContext *c, int step)
{
    constexpr int BATCH_SIZE = 128;

    for (int begin = 0; begin < TIMER_STEP_KV; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE, TIMER_STEP_KV);

        for (int i = begin; i < end; ++i)
        {
            std::string key = make_timer_key(step, i);
            redisAppendCommand(c, "GET %s", key.c_str());
        }

        for (int i = begin; i < end; ++i)
        {
            redisReply *reply = nullptr;
            if (redisGetReply(c, (void **)&reply) != REDIS_OK || reply == nullptr)
            {
                printf("==> FAILED -> resp-timer-expired step%d[%d], no reply\n", step, i);
                redisFree(c);
                exit(1);
            }
            if (reply->type != REDIS_REPLY_NIL)
            {
                printf("==> FAILED -> resp-timer-expired step%d[%d], key not expired (type=%d)\n", step, i, reply->type);
                freeReplyObject(reply);
                redisFree(c);
                exit(1);
            }
            freeReplyObject(reply);
        }
    }
}

// RESP mirror of testcase_timer_multi_step using SET ... EX 1 for expiration.
void resp_testcase_timer_multi_step(const char *ip, uint16_t port)
{
    redisContext *c = resp_connect(ip, port);

    resp_timer_set_batch(c, 0);
    printf("resp-timer-multi-step: t0 SET %d KV (1s TTL)\n", TIMER_STEP_KV);

    for (int step = 1; step <= TIMER_STEPS; ++step)
    {
        usleep(TIMER_STEP_INTERVAL_US);

        resp_timer_verify_expired(c, step - 1);
        resp_timer_set_batch(c, step);

        printf("resp-timer-multi-step: T%d verified step %d expired + SET %d new KV\n",
               step, step - 1, TIMER_STEP_KV);
    }

    redisFree(c);
    printf("==> PASSED -> resp-timer-multi-step (%d steps, %d KV/step, 1s TTL, 1.5s spacing)\n",
           TIMER_STEPS, TIMER_STEP_KV);
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
    int mode = atoi(argv[3]);

    // Modes 10-12 exercise the RESP protocol path through hiredis and use their
    // own client connection instead of the native KvClient.
    if (mode == 10)
    {
        resp_testcase_set_unique(argv[1], port);
        return 0;
    }
    else if (mode == 11)
    {
        resp_testcase_get_unique(argv[1], port);
        return 0;
    }
    else if (mode == 12)
    {
        resp_testcase_timer_multi_step(argv[1], port);
        return 0;
    }

    kv_client::KvClient client_ins(argv[1], port);

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
    else if (mode == 4)
        testcase_timeout(client_ins);
    else if (mode == 5)
        testcase_set(client_ins);
    else if (mode == 6)
        testcase_del(client_ins);
    else if (mode == 7)
        testcase_set_unique(client_ins);
    else if (mode == 8)
        testcase_get_unique(client_ins);
    else if (mode == 9)
        testcase_timer_multi_step(client_ins);
    else if (mode == 14)
        testcase_set_unique_first_half(client_ins);
    else if (mode == 15)
        testcase_set_unique_second_half(client_ins);
    else
    {
        perror("Invalid testcase number");
        return -1;
    }
    return 0;
}