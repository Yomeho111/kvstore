#include "kv_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>

namespace kv_config
{
    namespace
    {
        std::string trim(const std::string &text)
        {
            const char *spaces = " \t\r\n";
            size_t begin = text.find_first_not_of(spaces);
            if (begin == std::string::npos)
                return {};
            size_t end = text.find_last_not_of(spaces);
            return text.substr(begin, end - begin + 1);
        }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            return text;
        }

        bool parse_port(const std::string &text, uint16_t &out)
        {
            errno = 0;
            char *end = nullptr;
            long value = strtol(text.c_str(), &end, 10);

            if (errno != 0 || end == text.c_str() || (end && *end != '\0'))
                return false;
            if (value < 1 || value > 65535)
                return false;

            out = static_cast<uint16_t>(value);
            return true;
        }

        bool parse_role(const std::string &text, Role &out)
        {
            if (text == "master")
                out = Role::MASTER;
            else if (text == "slave" || text == "replica")
                out = Role::SLAVE;
            else if (text == "standalone" || text == "none")
                out = Role::STANDALONE;
            else
                return false;
            return true;
        }

        bool parse_persist_mode(const std::string &text, kv_persistent::PersistMode &out)
        {
            if (text == "aof")
                out = kv_persistent::PersistMode::AOF;
            else if (text == "rdb")
                out = kv_persistent::PersistMode::RDB;
            else if (text == "none" || text == "off")
                out = kv_persistent::PersistMode::NONE;
            else
                return false;
            return true;
        }
    } // namespace

    const char *role_name(Role role)
    {
        switch (role)
        {
            case Role::STANDALONE:
                return "standalone";
            case Role::MASTER:
                return "master";
            case Role::SLAVE:
                return "slave";
        }
        return "unknown";
    }

    const char *persist_mode_name(kv_persistent::PersistMode mode)
    {
        switch (mode)
        {
            case kv_persistent::PersistMode::NONE:
                return "none";
            case kv_persistent::PersistMode::AOF:
                return "aof";
            case kv_persistent::PersistMode::RDB:
                return "rdb";
        }
        return "unknown";
    }

    int load(const char *path, Config &out)
    {
        if (!path || !*path)
        {
            KV_ERROR("no configuration file given");
            return -1;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            KV_ERROR("cannot open configuration file '%s': %s", path, strerror(errno));
            return -1;
        }

        Config parsed;
        std::string section;
        std::string line;
        int line_no = 0;

        while (std::getline(file, line))
        {
            ++line_no;

            size_t comment = line.find_first_of("#;");
            if (comment != std::string::npos)
                line.erase(comment);

            line = trim(line);
            if (line.empty())
                continue;

            if (line.front() == '[')
            {
                if (line.back() != ']')
                {
                    KV_ERROR("%s:%d: malformed section header '%s'", path, line_no, line.c_str());
                    return -1;
                }
                section = lower(trim(line.substr(1, line.size() - 2)));
                continue;
            }

            size_t sep = line.find('=');
            if (sep == std::string::npos)
            {
                KV_ERROR("%s:%d: expected 'key = value', got '%s'", path, line_no, line.c_str());
                return -1;
            }

            std::string key = lower(trim(line.substr(0, sep)));
            std::string raw = trim(line.substr(sep + 1));
            std::string value = lower(raw);
            bool bad_value = false;

            if (section == "server" && key == "port")
                bad_value = !parse_port(value, parsed.port);
            else if (section == "server" && key == "log_level")
            {
                bool ok = false;
                parsed.log_level = kv_log::level_from_name(value.c_str(), &ok);
                bad_value = !ok;
            }
            else if (section == "persistence" && key == "mode")
                bad_value = !parse_persist_mode(value, parsed.persist_mode);
            else if (section == "replication" && key == "role")
                bad_value = !parse_role(value, parsed.role);
            else if (section == "replication" && key == "master_ip")
                parsed.master_ip = raw;
            else if (section == "replication" && key == "master_port")
                bad_value = !parse_port(value, parsed.master_port);
            else
            {
                KV_ERROR("%s:%d: unknown setting '%s' in section '[%s]'",
                         path, line_no, key.c_str(), section.c_str());
                return -1;
            }

            if (bad_value)
            {
                KV_ERROR("%s:%d: invalid value '%s' for '%s'", path, line_no, raw.c_str(), key.c_str());
                return -1;
            }
        }

        if (parsed.role == Role::SLAVE && parsed.master_ip.empty())
        {
            KV_ERROR("%s: replication.role is 'slave' but replication.master_ip is not set", path);
            return -1;
        }

        out = parsed;
        return 0;
    }
} // namespace kv_config
