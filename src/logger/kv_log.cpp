#include "kv_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <strings.h>
#include <time.h>

namespace kv_log
{
    void set_level(Level level)
    {
        g_level.store(static_cast<int>(level), std::memory_order_relaxed);
    }

    const char *level_name(Level level)
    {
        switch (level)
        {
            case Level::ERROR:
                return "ERROR";
            case Level::WARN:
                return "WARN";
            case Level::INFO:
                return "INFO";
            case Level::DEBUG:
                return "DEBUG";
        }
        return "UNKNOWN";
    }

    Level level_from_name(const char *name, bool *ok)
    {
        static const struct
        {
            const char *name;
            Level level;
        } table[] = {
            {"error", Level::ERROR},
            {"warn", Level::WARN},
            {"warning", Level::WARN},
            {"info", Level::INFO},
            {"debug", Level::DEBUG},
        };

        if (name)
        {
            for (const auto &entry : table)
            {
                if (strcasecmp(name, entry.name) == 0)
                {
                    if (ok)
                        *ok = true;
                    return entry.level;
                }
            }
        }

        if (ok)
            *ok = false;
        return Level::INFO;
    }

    void logf(Level level, const char *fmt, ...)
    {
        char stamp[32];
        struct timespec ts;
        struct tm tm_buf;

        if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && localtime_r(&ts.tv_sec, &tm_buf))
        {
            size_t used = strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf);
            snprintf(stamp + used, sizeof(stamp) - used, ".%03ld", ts.tv_nsec / 1000000);
        }
        else
        {
            stamp[0] = '\0';
        }

        // Keeps the prefix and the message together when threads log concurrently.
        flockfile(stderr);
        fprintf(stderr, "%s [%-5s] ", stamp, level_name(level));

        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);

        fputc('\n', stderr);
        funlockfile(stderr);
    }
} // namespace kv_log
