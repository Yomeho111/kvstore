#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <string>

#include "hiredis.h"

#define N 500000

#define TIME_SUB_MS(tv1, tv2) ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

// How many commands are pipelined before the replies are drained.
constexpr int BATCH_SIZE = 128;

constexpr int UNIQUE_KV_COUNT = 1000000;
constexpr size_t LARGE_VALUE_LEN = 13000;

// ===========================================================================
// RESP plumbing. Every testcase pipelines a batch of commands with
// redisAppendCommand and then drains the replies through the expect_* helpers,
// which abort the whole run on the first mismatch so scripts see a non-zero
// exit status.
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

static void resp_abort(redisContext *c, redisReply *reply, const char *casename, const char *detail)
{
    printf("==> FAILED -> %s, %s\n", casename, detail);
    if (reply)
        freeReplyObject(reply);
    redisFree(c);
    exit(1);
}

static redisReply *next_reply(redisContext *c, const char *casename)
{
    redisReply *reply = nullptr;
    if (redisGetReply(c, (void **)&reply) != REDIS_OK || reply == nullptr)
        resp_abort(c, reply, casename, c->errstr[0] ? c->errstr : "no reply");

    if (reply->type == REDIS_REPLY_ERROR)
        resp_abort(c, reply, casename, reply->str);

    return reply;
}

static void expect_status(redisContext *c, const char *casename, const char *expected)
{
    redisReply *reply = next_reply(c, casename);

    if (reply->type != REDIS_REPLY_STATUS || strcmp(reply->str, expected) != 0)
    {
        char detail[128];
        snprintf(detail, sizeof(detail), "expected status '%s' (type=%d)", expected, reply->type);
        resp_abort(c, reply, casename, detail);
    }

    freeReplyObject(reply);
}

static void expect_bulk(redisContext *c, const char *casename, const char *expected, size_t len)
{
    redisReply *reply = next_reply(c, casename);

    if (reply->type != REDIS_REPLY_STRING ||
        reply->len != len ||
        memcmp(reply->str, expected, len) != 0)
    {
        char detail[128];
        snprintf(detail, sizeof(detail), "unexpected bulk reply (type=%d, len=%zu, want %zu)",
                 reply->type, static_cast<size_t>(reply->len), len);
        resp_abort(c, reply, casename, detail);
    }

    freeReplyObject(reply);
}

static void expect_nil(redisContext *c, const char *casename)
{
    redisReply *reply = next_reply(c, casename);

    if (reply->type != REDIS_REPLY_NIL)
    {
        char detail[128];
        snprintf(detail, sizeof(detail), "expected nil (type=%d)", reply->type);
        resp_abort(c, reply, casename, detail);
    }

    freeReplyObject(reply);
}

static void expect_integer(redisContext *c, const char *casename, long long expected)
{
    redisReply *reply = next_reply(c, casename);

    if (reply->type != REDIS_REPLY_INTEGER || reply->integer != expected)
    {
        char detail[128];
        snprintf(detail, sizeof(detail), "expected integer %lld (type=%d)", expected, reply->type);
        resp_abort(c, reply, casename, detail);
    }

    freeReplyObject(reply);
}

// Used for cleanup commands whose result depends on what a previous run left behind.
static void skip_reply(redisContext *c, const char *casename)
{
    freeReplyObject(next_reply(c, casename));
}

// RESP has no MOD command: a SET onto an existing key falls back to modify()
// server-side, so the overwrite is what the second SET below exercises.
static long long testcase_basic(redisContext *c)
{
    const char *name = "resp-basic";

    redisAppendCommand(c, "DEL Teacher");
    redisAppendCommand(c, "SET Teacher King");
    redisAppendCommand(c, "GET Teacher");
    redisAppendCommand(c, "SET Teacher Darren");
    redisAppendCommand(c, "GET Teacher");
    redisAppendCommand(c, "EXISTS Teacher");
    redisAppendCommand(c, "DEL Teacher");
    redisAppendCommand(c, "GET Teacher");
    redisAppendCommand(c, "EXISTS Teacher");

    skip_reply(c, name);
    expect_status(c, name, "OK");
    expect_bulk(c, name, "King", 4);
    expect_status(c, name, "OK");
    expect_bulk(c, name, "Darren", 6);
    expect_integer(c, name, 1);
    expect_integer(c, name, 1);
    expect_nil(c, name);
    expect_integer(c, name, 0);

    return 9;
}

static const char *large_value()
{
    static char buffer[LARGE_VALUE_LEN];
    static bool ready = false;

    if (!ready)
    {
        for (size_t i = 0; i < sizeof(buffer); i++)
            buffer[i] = 'A' + (i % 26);
        ready = true;
    }

    return buffer;
}

static long long testcase_large_value(redisContext *c)
{
    const char *name = "resp-large-value";
    const char *value = large_value();

    redisAppendCommand(c, "DEL BigKey");
    redisAppendCommand(c, "SET BigKey %b", value, LARGE_VALUE_LEN);
    redisAppendCommand(c, "GET BigKey");
    redisAppendCommand(c, "SET BigKey %b", value, LARGE_VALUE_LEN);
    redisAppendCommand(c, "GET BigKey");
    redisAppendCommand(c, "EXISTS BigKey");
    redisAppendCommand(c, "DEL BigKey");
    redisAppendCommand(c, "GET BigKey");
    redisAppendCommand(c, "EXISTS BigKey");

    skip_reply(c, name);
    expect_status(c, name, "OK");
    expect_bulk(c, name, value, LARGE_VALUE_LEN);
    expect_status(c, name, "OK");
    expect_bulk(c, name, value, LARGE_VALUE_LEN);
    expect_integer(c, name, 1);
    expect_integer(c, name, 1);
    expect_nil(c, name);
    expect_integer(c, name, 0);

    return 9;
}

static long long testcase_timeout(redisContext *c)
{
    const char *name = "resp-timeout";

    redisAppendCommand(c, "DEL TimeoutSet TimeoutMod");
    redisAppendCommand(c, "SET TimeoutSet Alpha PX 200");
    redisAppendCommand(c, "GET TimeoutSet");
    redisAppendCommand(c, "SET TimeoutMod Before");
    redisAppendCommand(c, "SET TimeoutMod After PX 300");
    redisAppendCommand(c, "GET TimeoutMod");
    redisAppendCommand(c, "EXISTS TimeoutSet");
    redisAppendCommand(c, "EXISTS TimeoutMod");

    skip_reply(c, name);
    expect_status(c, name, "OK");
    expect_bulk(c, name, "Alpha", 5);
    expect_status(c, name, "OK");
    expect_status(c, name, "OK");
    expect_bulk(c, name, "After", 5);
    expect_integer(c, name, 1);
    expect_integer(c, name, 1);

    usleep(600 * 1000);

    redisAppendCommand(c, "GET TimeoutSet");
    redisAppendCommand(c, "EXISTS TimeoutSet");
    redisAppendCommand(c, "GET TimeoutMod");
    redisAppendCommand(c, "EXISTS TimeoutMod");

    expect_nil(c, name);
    expect_integer(c, name, 0);
    expect_nil(c, name);
    expect_integer(c, name, 0);

    return 12;
}

static const char *bulk_value()
{
    return "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
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
}

static long long testcase_set(redisContext *c)
{
    const char *name = "resp-set";
    const char *value = bulk_value();
    const size_t value_len = strlen(value);

    for (int begin = 1; begin <= N; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE - 1, N);

        for (int i = begin; i <= end; ++i)
            redisAppendCommand(c, "SET key%d %b", i, value, value_len);

        for (int i = begin; i <= end; ++i)
            expect_status(c, name, "OK");
    }

    return N;
}

static long long testcase_del(redisContext *c)
{
    const char *name = "resp-del";

    for (int begin = 1; begin <= N; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE - 1, N);

        for (int i = begin; i <= end; ++i)
            redisAppendCommand(c, "DEL key%d", i);

        for (int i = begin; i <= end; ++i)
            expect_integer(c, name, 1);
    }

    return N;
}

static std::string make_unique_key(int i)
{
    return "ukey" + std::to_string(i);
}

static std::string make_unique_value(int i)
{
    return "uval" + std::to_string(i);
}

// SET the unique pairs in [first, last], each with a distinct key and value.
static long long set_unique_range(redisContext *c, const char *name, int first, int last)
{
    for (int begin = first; begin <= last; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE - 1, last);

        for (int i = begin; i <= end; ++i)
        {
            std::string key = make_unique_key(i);
            std::string value = make_unique_value(i);
            redisAppendCommand(c, "SET %b %b", key.data(), key.size(), value.data(), value.size());
        }

        for (int i = begin; i <= end; ++i)
            expect_status(c, name, "OK");
    }

    return last - first + 1;
}

static long long testcase_set_unique(redisContext *c)
{
    return set_unique_range(c, "resp-set-unique", 1, UNIQUE_KV_COUNT);
}

static long long testcase_set_unique_first_half(redisContext *c)
{
    return set_unique_range(c, "resp-set-unique-first-half", 1, UNIQUE_KV_COUNT / 2);
}

static long long testcase_set_unique_second_half(redisContext *c)
{
    return set_unique_range(c, "resp-set-unique-second-half", UNIQUE_KV_COUNT / 2 + 1, UNIQUE_KV_COUNT);
}

// GET the pairs written by testcase_set_unique and verify every value.
static long long testcase_get_unique(redisContext *c)
{
    const char *name = "resp-get-unique";

    for (int begin = 1; begin <= UNIQUE_KV_COUNT; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE - 1, UNIQUE_KV_COUNT);

        for (int i = begin; i <= end; ++i)
        {
            std::string key = make_unique_key(i);
            redisAppendCommand(c, "GET %b", key.data(), key.size());
        }

        for (int i = begin; i <= end; ++i)
        {
            std::string expected = make_unique_value(i);
            expect_bulk(c, name, expected.data(), expected.size());
        }
    }

    return UNIQUE_KV_COUNT;
}

// ---------------------------------------------------------------------------
// Multi-step timer / expiration test.
//
//   t0        : SET TIMER_STEP_KV unique KV, each with a 1s expiration.
//   T1 .. T5  : 1.5s after the previous step, GET the previous step's batch
//               (every key must have expired -> nil) and then SET a fresh batch
//               of TIMER_STEP_KV unique KV, again with a 1s TTL.
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

// SET the whole batch for `step`, each key carrying a 1s expiry.
static long long timer_set_batch(redisContext *c, int step)
{
    const char *name = "resp-timer-set";

    for (int begin = 0; begin < TIMER_STEP_KV; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE, TIMER_STEP_KV);

        for (int i = begin; i < end; ++i)
        {
            std::string key = make_timer_key(step, i);
            std::string value = make_timer_value(step, i);
            redisAppendCommand(c, "SET %b %b EX 1", key.data(), key.size(), value.data(), value.size());
        }

        for (int i = begin; i < end; ++i)
            expect_status(c, name, "OK");
    }

    return TIMER_STEP_KV;
}

// GET the whole batch for `step` and assert every key has expired.
static long long timer_verify_expired(redisContext *c, int step)
{
    const char *name = "resp-timer-expired";

    for (int begin = 0; begin < TIMER_STEP_KV; begin += BATCH_SIZE)
    {
        int end = std::min(begin + BATCH_SIZE, TIMER_STEP_KV);

        for (int i = begin; i < end; ++i)
        {
            std::string key = make_timer_key(step, i);
            redisAppendCommand(c, "GET %b", key.data(), key.size());
        }

        for (int i = begin; i < end; ++i)
            expect_nil(c, name);
    }

    return TIMER_STEP_KV;
}

static long long testcase_timer_multi_step(redisContext *c)
{
    // t0: seed the first batch.
    long long commands = timer_set_batch(c, 0);
    printf("timer-multi-step: t0 SET %d KV (1s TTL)\n", TIMER_STEP_KV);

    // T1 .. T5: wait past the TTL, confirm the previous batch expired, seed the next.
    for (int step = 1; step <= TIMER_STEPS; ++step)
    {
        usleep(TIMER_STEP_INTERVAL_US);

        commands += timer_verify_expired(c, step - 1);
        commands += timer_set_batch(c, step);

        printf("timer-multi-step: T%d verified step %d expired + SET %d new KV\n",
               step, step - 1, TIMER_STEP_KV);
    }

    return commands;
}

using TestcaseFn = long long (*)(redisContext *);

static void report(const char *name, long long commands, const struct timeval &begin)
{
    struct timeval end;
    gettimeofday(&end, nullptr);

    long long time_used = TIME_SUB_MS(end, begin); // ms
    if (time_used <= 0)
        time_used = 1;

    printf("==> PASSED -> %s, commands: %lld, time_used: %lld ms, qps: %lld\n",
           name, commands, time_used, commands * 1000 / time_used);
}

static void run_testcase(redisContext *c, TestcaseFn func, const char *name)
{
    struct timeval begin;
    gettimeofday(&begin, nullptr);

    report(name, func(c), begin);
}

// Replays a whole testcase `count` times and reports the aggregate throughput.
static void repeat_testcase(redisContext *c, TestcaseFn func, const char *name)
{
    constexpr int count = 10000;

    struct timeval begin;
    gettimeofday(&begin, nullptr);

    long long commands = 0;
    for (int i = 0; i < count; i++)
        commands += func(c);

    char label[128];
    snprintf(label, sizeof(label), "%s x%d", name, count);
    report(label, commands, begin);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <ip> <port> <mode>\n"
            "\n"
            "  0   basic SET/GET/overwrite/EXISTS/DEL sequence\n"
            "  1   mode 0 replayed 10k times, with throughput\n"
            "  2   the same sequence with a %zu byte value\n"
            "  3   mode 2 replayed 10k times, with throughput\n"
            "  4   TTL: keys set with PX must be gone once they expire\n"
            "  5   SET %d keys sharing one 1 KiB value\n"
            "  6   DEL the %d keys written by mode 5\n"
            "  7   SET %d unique key/value pairs\n"
            "  8   GET and verify the pairs written by mode 7\n"
            "  9   multi-step 1s TTL expiration under sustained writes\n"
            "  10  SET the first half of the mode 7 pairs\n"
            "  11  SET the second half of the mode 7 pairs\n",
            prog, LARGE_VALUE_LEN, N, N, UNIQUE_KV_COUNT);
}

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        usage(argv[0]);
        return -1;
    }

    uint16_t port = atoi(argv[2]);
    int mode = atoi(argv[3]);

    redisContext *c = resp_connect(argv[1], port);

    switch (mode)
    {
        case 0:
            run_testcase(c, testcase_basic, "resp-basic");
            break;
        case 1:
            repeat_testcase(c, testcase_basic, "resp-basic");
            break;
        case 2:
            run_testcase(c, testcase_large_value, "resp-large-value");
            break;
        case 3:
            repeat_testcase(c, testcase_large_value, "resp-large-value");
            break;
        case 4:
            run_testcase(c, testcase_timeout, "resp-timeout");
            break;
        case 5:
            run_testcase(c, testcase_set, "resp-set");
            break;
        case 6:
            run_testcase(c, testcase_del, "resp-del");
            break;
        case 7:
            run_testcase(c, testcase_set_unique, "resp-set-unique");
            break;
        case 8:
            run_testcase(c, testcase_get_unique, "resp-get-unique");
            break;
        case 9:
            run_testcase(c, testcase_timer_multi_step, "resp-timer-multi-step");
            break;
        case 10:
            run_testcase(c, testcase_set_unique_first_half, "resp-set-unique-first-half");
            break;
        case 11:
            run_testcase(c, testcase_set_unique_second_half, "resp-set-unique-second-half");
            break;
        default:
            usage(argv[0]);
            redisFree(c);
            return -1;
    }

    redisFree(c);
    return 0;
}