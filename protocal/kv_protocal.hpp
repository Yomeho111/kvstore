#ifndef __KV_PROTOCAL_HPP
#define __KV_PROTOCAL_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string>
#include <cctype>

#include "status.h"
#include "engine_interface.hpp"
#include "status.h"
#include "rbtree_engine/rbtree_engine.h"
#include "array_engine/array_engine.h"
#include "hash_engine/hash_engine.h"
#include "skiplist_engine/skiplist_engine.h"
#include "allocator.h"
#include "kv_header.h"
#include "kv_persistent.h"
#include "replicate.h"
#include "kv_log.h"
#include "thread_pool.hpp"

// #define MAX_BODY_SIZE 4096
#define MAX_TOKEN_SIZE 2
#define BUFFER_SIZE 128

namespace kv_protocal
{

    struct DataField
    {
        char *data{nullptr};
        size_t size{0};
    };

    constexpr uint64_t cmd_tag(const char *s, size_t len)
    {
        if (len == 0 || len > 8)
            return 0;
        uint64_t tag = 0;
        for (size_t i = 0; i < len; i++)
            tag |= static_cast<uint64_t>(static_cast<unsigned char>(s[i]) & 0xDF) << (i * 8);
        return tag;
    }

    template <size_t N>
    constexpr uint64_t cmd_tag(const char (&s)[N])
    {
        return cmd_tag(s, N - 1);
    }

    inline constexpr const uint64_t KV_PING = cmd_tag("PING");
    inline constexpr const uint64_t KV_SET = cmd_tag("SET");
    inline constexpr const uint64_t KV_SETEX = cmd_tag("SETEX");
    inline constexpr const uint64_t KV_PSETEX = cmd_tag("PSETEX");
    inline constexpr const uint64_t KV_EXPIRE = cmd_tag("EXPIRE");
    inline constexpr const uint64_t KV_PEXPIRE = cmd_tag("PEXPIRE");
    inline constexpr const uint64_t KV_GET = cmd_tag("GET");
    inline constexpr const uint64_t KV_DEL = cmd_tag("DEL");
    inline constexpr const uint64_t KV_EXISTS = cmd_tag("EXISTS");
    inline constexpr const uint64_t KV_QUIT = cmd_tag("QUIT");
    inline constexpr const uint64_t KV_SELECT = cmd_tag("SELECT");
    inline constexpr const uint64_t KV_CLIENT = cmd_tag("CLIENT");
    inline constexpr const uint64_t KV_COMMAND = cmd_tag("COMMAND");
    inline constexpr const uint64_t KV_CONFIG = cmd_tag("CONFIG");
    inline constexpr const uint64_t KV_SAVE = cmd_tag("SAVE");
    inline constexpr const uint64_t KV_SYNC = cmd_tag("SYNC");
    inline constexpr const uint64_t KV_SYNCFIN = cmd_tag("SYNCFIN");

    inline constexpr const char *SYNCFIN_RESP = "*1\r\n$7\r\nSYNCFIN\r\n";

    // Where a command's reply goes. A replica applies commands without
    // answering, so it passes a default-constructed (discarding) sink and the
    // reply is never assembled.
    class RespSink
    {
    public:
        RespSink() = default;
        RespSink(string &out) : _out(&out) {}

        RespSink &operator+=(const char *s)
        {
            if (_out)
                *_out += s;
            return *this;
        }

        void append(const char *data, size_t len)
        {
            if (_out)
                _out->append(data, len);
        }

    private:
        string *_out{nullptr};
    };

    template <typename KvEngine>
    class KvProtocal
    {
    public:
        static KvProtocal &instance()
        {
            static KvProtocal prot;
            static int ret = prot._engine.init();
            if (ret < 0)
            {
                fprintf(stderr, "corrupted database\n");
                exit(0);
            }
            return prot;
        }

        int process_num_request(struct network::StatusM *status, uint32_t num_request)
        {
            if (status == nullptr || num_request == 0)
                return -1;

            status->status = network::READ_HEADER;
            status->num_request = num_request;
            status->req_info = (HeaderInfo *)allocator::kv_malloc(num_request * sizeof(HeaderInfo));
            if (status->req_info == nullptr)
                return -2;

            memset(status->req_info, 0, num_request * sizeof(HeaderInfo));
            return 0;
        }

        int process_header(struct network::StatusM *status, struct ::iovec **r_iovec)
        {
            if (status == nullptr || status->status != network::READ_HEADER || status->req_info == nullptr || status->num_request == 0)
                return -1;

            *r_iovec = (struct ::iovec *)allocator::kv_malloc(status->num_request * sizeof(struct ::iovec));
            if (*r_iovec == nullptr)
                return -2;
            memset(*r_iovec, 0, status->num_request * sizeof(struct ::iovec));

            bool is_response{false};

            for (int i = 0; i < status->num_request; i++)
            {
                if (status->req_info[i].command == KVS_START || status->req_info[i].command >= KVS_END)
                {
                    status->req_info[i].command = KVS_INVALID;
                }
                else
                {
                    if (status->req_info[i].command == KVS_RESP)
                        is_response = true;
                    (*r_iovec)[i].iov_len = status->req_info[i].body_length;
                    (*r_iovec)[i].iov_base = allocator::kv_malloc(status->req_info[i].body_length);
                    if ((*r_iovec)[i].iov_base == nullptr)
                        return -2;
                }
            }
            status->is_response = is_response;
            status->status = network::READ_BODY;
            return 0;
        }

        int process_body(struct network::StatusM *status, struct ::iovec *r_iovec, struct ::iovec **w_iovec)
        {
            if (status == nullptr || r_iovec == nullptr || w_iovec == nullptr)
                return -1;

            if (status->is_response)
            {
                status->is_response = false;
                status->status = network::READ_NUM_REQUEST;
                return 0;
            }

            size_t iovec_size = (status->num_request + 2);
            status->w_iovec_size = iovec_size;
            size_t w_iovec_size = iovec_size * sizeof(struct ::iovec);

            *w_iovec = static_cast<struct ::iovec *>(allocator::kv_malloc(w_iovec_size));
            if (*w_iovec == nullptr)
                return -2;
            memset(*w_iovec, 0, w_iovec_size);

            (*w_iovec)[0].iov_len = NUM_HEADER_SIZE;
            (*w_iovec)[0].iov_base = allocator::kv_malloc(NUM_HEADER_SIZE);
            if ((*w_iovec)[0].iov_base == nullptr)
                return -2;

            ((NumHeader *)(*w_iovec)[0].iov_base)->tag = NUM_HEADER_TAG;
            ((NumHeader *)(*w_iovec)[0].iov_base)->num_request = status->num_request;

            (*w_iovec)[1].iov_len = sizeof(HeaderInfo) * status->num_request;
            (*w_iovec)[1].iov_base = allocator::kv_malloc(sizeof(HeaderInfo) * status->num_request);
            if ((*w_iovec)[1].iov_base == nullptr)
                return -2;

            memset((*w_iovec)[1].iov_base, 0, sizeof(HeaderInfo) * status->num_request);

            for (int i = 0; i < status->num_request; i++)
            {
                char *response_body = nullptr;
                int wbuf_size = _process_body(status->req_info[i].command, status->req_info[i].body_length, status->req_info[i].key_length, static_cast<char *>(r_iovec[i].iov_base), &response_body, &status->req_info[i].timeout);
                if (wbuf_size <= 0)
                    continue;

                (*w_iovec)[i + 2].iov_base = response_body;
                (*w_iovec)[i + 2].iov_len = wbuf_size;
                ((HeaderInfo *)(*w_iovec)[1].iov_base)[i].body_length = wbuf_size;
                ((HeaderInfo *)(*w_iovec)[1].iov_base)[i].command = KVS_RESP;
            }

            status->status = network::SEND_RESPONSE;
            return 0;
        }

        // Execute one already-parsed RESP (Redis protocol) command.
        // argv/argvlen hold the command name and its arguments (parsed by the
        // hiredis reader in the reactor). The RESP-encoded reply is appended to
        // `out`. Returns 0 normally, or 1 if the connection should be closed
        // after the reply is sent (QUIT).
        int process_resp_command(int argc, char **argv, size_t *argvlen, const uint64_t cmd, RespSink out)
        {
            if (argc <= 0 || argv == nullptr || argvlen == nullptr || argv[0] == nullptr)
            {
                out += "-ERR invalid request\r\n";
                return 0;
            }

            switch (cmd)
            {
                case KV_PING:
                {
                    if (argc >= 2)
                        _resp_append_bulk(out, argv[1], argvlen[1]);
                    else
                        out += "+PONG\r\n";
                    return 0;
                }
                case KV_SET:
                {
                    if (argc < 3)
                    {
                        out += "-ERR wrong number of arguments for 'set' command\r\n";
                        return 0;
                    }
                    // Optional expiry: SET key value [EX seconds | PX milliseconds].
                    TimeoutSpec ts;
                    bool has_expiry = false;
                    for (int i = 3; i < argc;)
                    {
                        char opt[8] = {0};
                        size_t ol = argvlen[i] < sizeof(opt) - 1 ? argvlen[i] : sizeof(opt) - 1;
                        for (size_t j = 0; j < ol; j++)
                            opt[j] = static_cast<char>(::toupper(static_cast<unsigned char>(argv[i][j])));
                        bool is_ex = strcmp(opt, "EX") == 0;
                        bool is_px = strcmp(opt, "PX") == 0;
                        if ((is_ex || is_px) && i + 1 < argc)
                        {
                            long long amt;
                            if (!_resp_to_ll(argv[i + 1], argvlen[i + 1], amt) || amt <= 0)
                            {
                                out += "-ERR invalid expire time in 'set' command\r\n";
                                return 0;
                            }
                            if (is_ex)
                                _fill_timeout_sec(ts, amt);
                            else
                                _fill_timeout_ms(ts, amt);
                            has_expiry = true;
                            i += 2;
                        }
                        else
                        {
                            out += "-ERR syntax error\r\n";
                            return 0;
                        }
                    }
                    // Redis SET overwrites an existing key. The engine's set()
                    // refuses to replace one (returns > 0), so fall back to modify().
                    TimeoutSpec *tp = has_expiry ? &ts : nullptr;
                    int ret = _engine.set(argv[1], argvlen[1], argv[2], argvlen[2], tp);
                    if (ret > 0)
                        ret = _engine.modify(argv[1], argvlen[1], argv[2], argvlen[2], tp);
                    if (ret == 0)
                        out += "+OK\r\n";
                    else
                        out += "-ERR set failed\r\n";
                    return 0;
                }
                case KV_SETEX:
                case KV_PSETEX:
                {
                    // SETEX key seconds value / PSETEX key milliseconds value
                    if (argc < 4)
                    {
                        out += "-ERR wrong number of arguments for 'setex' command\r\n";
                        return 0;
                    }
                    long long amt;
                    if (!_resp_to_ll(argv[2], argvlen[2], amt) || amt <= 0)
                    {
                        out += "-ERR invalid expire time in 'setex' command\r\n";
                        return 0;
                    }
                    TimeoutSpec ts;
                    if (cmd == KV_PSETEX)
                        _fill_timeout_ms(ts, amt);
                    else
                        _fill_timeout_sec(ts, amt);
                    int ret = _engine.set(argv[1], argvlen[1], argv[3], argvlen[3], &ts);
                    if (ret > 0)
                        ret = _engine.modify(argv[1], argvlen[1], argv[3], argvlen[3], &ts);
                    if (ret == 0)
                        out += "+OK\r\n";
                    else
                        out += "-ERR setex failed\r\n";
                    return 0;
                }
                case KV_EXPIRE:
                case KV_PEXPIRE:
                {
                    // EXPIRE key seconds / PEXPIRE key milliseconds
                    if (argc < 3)
                    {
                        out += "-ERR wrong number of arguments for 'expire' command\r\n";
                        return 0;
                    }
                    long long amt;
                    if (!_resp_to_ll(argv[2], argvlen[2], amt))
                    {
                        out += "-ERR value is not an integer or out of range\r\n";
                        return 0;
                    }
                    // A non-positive expiry deletes the key immediately (Redis semantics).
                    if (amt <= 0)
                    {
                        int d = _engine.del(argv[1], argvlen[1]);
                        _resp_append_int(out, d == 0 ? 1 : 0);
                        return 0;
                    }
                    // Re-apply the current value with a timeout to schedule expiry.
                    char *value = nullptr;
                    int r = _engine.get(argv[1], argvlen[1], &value);
                    if (r <= 0)
                    {
                        _resp_append_int(out, 0); // key does not exist
                        return 0;
                    }
                    TimeoutSpec ts;
                    if (cmd == KV_PEXPIRE)
                        _fill_timeout_ms(ts, amt);
                    else
                        _fill_timeout_sec(ts, amt);
                    int ret = _engine.modify(argv[1], argvlen[1], value, static_cast<size_t>(r) - 2, &ts);
                    allocator::kv_free(value);
                    _resp_append_int(out, ret == 0 ? 1 : 0);
                    return 0;
                }
                case KV_GET:
                {
                    if (argc < 2)
                    {
                        out += "-ERR wrong number of arguments for 'get' command\r\n";
                        return 0;
                    }
                    char *value = nullptr;
                    int ret = _engine.get(argv[1], argvlen[1], &value);
                    if (ret > 0)
                    {
                        // get() returns the value followed by a trailing "\r\n" and a
                        // length of value_size + 2; we reuse that "\r\n" as the RESP
                        // bulk-string terminator.
                        size_t vlen = static_cast<size_t>(ret) - 2;
                        char hdr[32];
                        int n = snprintf(hdr, sizeof(hdr), "$%zu\r\n", vlen);
                        out.append(hdr, n);
                        out.append(value, ret);
                        allocator::kv_free(value);
                    }
                    else if (ret == 0)
                        out += "$-1\r\n"; // nil
                    else
                        out += "-ERR get failed\r\n";
                    return 0;
                }
                case KV_DEL:
                {
                    if (argc < 2)
                    {
                        out += "-ERR wrong number of arguments for 'del' command\r\n";
                        return 0;
                    }
                    int deleted = 0;
                    for (int i = 1; i < argc; i++)
                        if (_engine.del(argv[i], argvlen[i]) == 0)
                            deleted++;
                    _resp_append_int(out, deleted);
                    return 0;
                }
                case KV_EXISTS:
                {
                    if (argc < 2)
                    {
                        out += "-ERR wrong number of arguments for 'exists' command\r\n";
                        return 0;
                    }
                    int found = 0;
                    for (int i = 1; i < argc; i++)
                        if (_engine.exist(argv[i], argvlen[i]) == 0)
                            found++;
                    _resp_append_int(out, found);
                    return 0;
                }
                case KV_QUIT:
                    out += "+OK\r\n";
                    return 1;
                case KV_SELECT:
                case KV_CLIENT:
                    // Not implemented, but reply OK so redis-cli / redis-benchmark
                    // can complete their handshake.
                    out += "+OK\r\n";
                    return 0;
                case KV_COMMAND:
                case KV_CONFIG:
                    out += "*0\r\n"; // empty array
                    return 0;
                case KV_SAVE:
                {
                    if (kv_persistent::g_persist_mode == kv_persistent::PersistMode::RDB)
                    {
                        string tmp_file_path{kv_persistent::RDB_TMP};
                        tmp_file_path += "_";

                        auto counter = std::to_string(_tmp_file_counter++);
                        tmp_file_path.append(counter.data(), counter.size());

                        auto &thread_pool = base_component::ThreadPool::instance();

                        int ret = thread_pool.submit(
                            [this, tmp_file_path = std::move(tmp_file_path)]
                            {
                                int ret = _engine.save(tmp_file_path);
                                (void)ret;
                            });
                        out += ret == 0 ? "+OK\r\n" : "-ERR submit save task failed\r\n";
                        return 0;
                    }
                    out += "-ERR Current persistent mode is not rdb\r\n";
                    return 0;
                }
                case KV_SYNC:
                {
                    if (!replicate::g_is_master)
                    {
                        out += "-ERR This is not a master server\r\n";
                        return 0;
                    }

                    // SYNC <replica-rdma-ip> <replica-rdma-port> <slave-tcp ip> <slave-tcp-port>: snapshot the dataset and
                    // push it over RDMA. The replica must already be listening.
                    if (argc < 5)
                    {
                        out += "-ERR wrong number of arguments for 'SYNC' command\r\n";
                        return 0;
                    }

                    // hiredis NUL-terminates every bulk argument, so the address goes
                    // straight to inet_pton() with no copy; reject embedded NULs.
                    if (argv[1] == nullptr || argvlen[1] == 0 || strlen(argv[1]) != argvlen[1])
                    {
                        out += "-ERR invalid replica address\r\n";
                        return 0;
                    }

                    if (argv[3] == nullptr || argvlen[3] == 0 || strlen(argv[3]) != argvlen[3])
                    {
                        out += "-ERR invalid slave tcp address\r\n";
                        return 0;
                    }

                    KV_INFO("The slave info:\nrdma: %s: %s\ntcp: %s: %s", argv[1], argv[2], argv[3], argv[4]);

                    long long port = 0;
                    if (!_resp_to_ll(argv[2], argvlen[2], port) || port <= 0 || port > 65535)
                    {
                        out += "-ERR invalid replica port\r\n";
                        return 0;
                    }

                    string tmp_file_path{kv_persistent::RDB_TMP};
                    tmp_file_path += "_";

                    auto counter = std::to_string(_tmp_file_counter++);
                    tmp_file_path.append(counter.data(), counter.size());

                    string replica_ip{argv[1], argvlen[1]};
                    uint16_t replica_port = static_cast<uint16_t>(port);

                    auto &thread_pool = base_component::ThreadPool::instance();

                    int ret = thread_pool.submit(
                        [this,
                         replica_ip = std::move(replica_ip),
                         replica_port,
                         tmp_file_path = std::move(tmp_file_path)]
                        {
                            int result = _send_replicate(
                                replica_ip,
                                replica_port,
                                tmp_file_path);

                            (void)result;
                        });

                    out += ret == 0
                               ? "+OK\r\n"
                               : "-ERR submit sync task failed\r\n";
                    return 0;
                }
                case KV_SYNCFIN:
                    return 0;
                default:
                    break;
            }

            out += "-ERR unknown command\r\n";
            return 0;
        }

        int size()
        {
            return _engine.size();
        }

        int load_snapshot(const string &file_path_str, bool to_disk)
        {
            return _engine.load_snapshot(file_path_str, to_disk);
        }

        template <typename... Args>
        DataField make_command(const Args &...vars)
        {
            constexpr size_t argc = sizeof...(Args);

            if constexpr (argc == 0)
            {
                return {};
            }
            else
            {
                size_t buffer_size = kv_persistent::get_resp_size(vars.size()...);

                char *buffer = (char *)allocator::kv_malloc(buffer_size);
                if (!buffer)
                {
                    return {};
                }

                if (kv_persistent::format_resp(buffer, vars...) < 0)
                {
                    allocator::kv_free(buffer);
                    return {};
                }

                return {buffer, buffer_size};
            }
        }

    private:
        KvProtocal() {}
        ~KvProtocal() {}

        static void _resp_append_int(RespSink &out, long long v)
        {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), ":%lld\r\n", v);
            out.append(buf, n);
        }

        static void _resp_append_bulk(RespSink &out, const char *data, size_t len)
        {
            char hdr[32];
            int n = snprintf(hdr, sizeof(hdr), "$%zu\r\n", len);
            out.append(hdr, n);
            out.append(data, len);
            out.append("\r\n", 2);
        }

        // Parse a base-10 integer argument (bounded to avoid overflow). Returns
        // false on any non-numeric input.
        static bool _resp_to_ll(const char *s, size_t len, long long &out)
        {
            if (s == nullptr || len == 0 || len > 18)
                return false;
            long long v = 0;
            size_t i = 0;
            bool neg = false;
            if (s[0] == '-')
            {
                neg = true;
                i = 1;
            }
            else if (s[0] == '+')
            {
                i = 1;
            }
            if (i == len)
                return false;
            for (; i < len; i++)
            {
                if (s[i] < '0' || s[i] > '9')
                    return false;
                v = v * 10 + (s[i] - '0');
            }
            out = neg ? -v : v;
            return true;
        }

        // Fill a TimeoutSpec (interpreted by the engine as a relative TTL) from
        // a whole number of seconds or milliseconds.
        static void _fill_timeout_sec(TimeoutSpec &ts, long long seconds)
        {
            ts.tv_sec = static_cast<long>(seconds);
            ts.tv_nsec = 0;
        }

        static void _fill_timeout_ms(TimeoutSpec &ts, long long ms)
        {
            ts.tv_sec = static_cast<long>(ms / 1000);
            ts.tv_nsec = static_cast<long>((ms % 1000) * 1000000);
        }

        /**
         * @brief the key function to save all data into temp file and send to slave server
         */
        int _send_replicate(const string &ip, uint16_t port, const string &tmp_file_path)
        {
            // save all data into tmp file
            if (_engine.save(tmp_file_path, true) < 0)
            {
                return -1;
            }

            // launch rdma server and send the tmp file
            replicate::MasterServer master(ip.c_str(), port);
            if (master.init() < 0)
                return -2;

            KV_INFO("Master rdma init succeed");
            int ret = master.send((string{kv_persistent::RDB_FOLDER} + tmp_file_path).c_str());
            if (ret == 1)
                KV_INFO("Master has no data");
            else if (ret < 0)
                return -3;

            KV_INFO("Master rdma send succeed");
            // close rdma and remove tmp file
            _engine.remove_temp_file(tmp_file_path);
            KV_INFO("remove temp file");
            return 0;
        }

        int _split_token(char *body, char **tokens, uint32_t key_length)
        {
            if (body == nullptr || tokens == nullptr || key_length == 0)
                return -1;

            tokens[0] = body;
            tokens[1] = body + key_length;
            return 0;
        }

        int _process_body(uint16_t command, size_t body_length, size_t key_length, char *body, char **response, struct TimeoutSpec *timeout)
        {
            if (body == nullptr || body_length == 0 || key_length == 0) // body_length > MAX_BODY_SIZE
                return -1;

            char *tokens[MAX_TOKEN_SIZE] = {0};

            uint32_t value_length = body_length - key_length;

            int count = _split_token(body, tokens, key_length);
            if (count == -1)
                return -1;

            int wbuf_size = _process_tokens(tokens, response, command, key_length, value_length, timeout);
            if (wbuf_size == -1)
                return -1;

            return wbuf_size;
        }

        int _process_tokens(char **tokens, char **response, uint16_t command, uint32_t key_length, uint32_t value_length, struct TimeoutSpec *timeout)
        {
            if (tokens == nullptr || response == nullptr)
                return -1;

            int wbuf_size = 0;
            char wbuf[BUFFER_SIZE] = {0};
            int ret = 0;
            char *key = tokens[0];
            char *value = tokens[1];

            switch (command)
            {
                case KVS_SET:
                {
                    ret = _engine.set(key, key_length, value, value_length, timeout);
                    if (ret == 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "OK\r\n");
                    else if (ret < 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "ERROR\r\n");
                    else if (ret > 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "EXIST\r\n");
                    break;
                }
                case KVS_GET:
                {
                    ret = _engine.get(key, key_length, &value);
                    if (ret > 0)
                        wbuf_size = ret;
                    else if (ret < 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "ERROR\r\n");
                    else if (ret == 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "NOT EXIST\r\n");
                    break;
                }
                case KVS_MOD:
                {
                    ret = _engine.modify(key, key_length, value, value_length, timeout);
                    if (ret == 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "OK\r\n");
                    else if (ret < 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "ERROR\r\n");
                    else if (ret > 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "NOT EXIST\r\n");
                    break;
                }
                case KVS_DEL:
                {
                    ret = _engine.del(key, key_length);
                    if (ret == 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "OK\r\n");
                    else if (ret < 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "ERROR\r\n");
                    else if (ret > 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "NOT EXIST\r\n");
                    break;
                }
                case KVS_EXIST:
                {
                    ret = _engine.exist(key, key_length);
                    if (ret == 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "EXIST\r\n");
                    else if (ret < 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "ERROR\r\n");
                    else if (ret > 0)
                        wbuf_size = snprintf(wbuf, BUFFER_SIZE, "NOT EXIST\r\n");
                    break;
                }
                default:
                    wbuf_size = snprintf(wbuf, BUFFER_SIZE, "Invalid Command\r\n");
                    break;
            }

            // *response = (char *)allocator::kv_malloc(wbuf_size);

            if (command == KVS_GET && ret > 0)
                *response = value;
            else
            {
                *response = (char *)allocator::kv_malloc(wbuf_size);
                if (*response == nullptr)
                    return -2;
                memcpy(*response, wbuf, wbuf_size);
            }
            return static_cast<uint32_t>(wbuf_size);
        }

        KvProtocal(const KvProtocal &) = delete;
        KvProtocal(KvProtocal &&) = delete;

        KvProtocal &operator=(const KvProtocal &) = delete;
        KvProtocal &operator=(KvProtocal &&) = delete;

        KvEngine _engine;
        size_t _tmp_file_counter{0};
    };

#ifdef RBTREE_ENGINE
    using KvStoreProtocal = KvProtocal<kv_engine::RbtreeEngine>;
#elif defined(ARRAY_ENGINE)
    using KvStoreProtocal = KvProtocal<kv_engine::ArrayEngine>;
#elif defined(HASH_ENGINE)
    using KvStoreProtocal = KvProtocal<kv_engine::HashEngine>;
#elif defined(SKIPLIST_ENGINE)
    using KvStoreProtocal = KvProtocal<kv_engine::SkiplistEngine>;
#else
    using KvStoreProtocal = KvProtocal<kv_engine::RbtreeEngine>;
#endif
} // namespace kv_protocal

#endif // __KV_PROTOCAL_HPP