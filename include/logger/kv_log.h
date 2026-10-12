#ifndef __KV_LOG_H
#define __KV_LOG_H

#include <atomic>

namespace kv_log
{
    enum class Level : int
    {
        ERROR = 0,
        WARN = 1,
        INFO = 2,
        DEBUG = 3,
    };

    // Read on every log call, so it is kept relaxed and lock-free.
    inline std::atomic<int> g_level{static_cast<int>(Level::INFO)};

    void set_level(Level level);

    const char *level_name(Level level);

    // Falls back to INFO and clears *ok when the name is not recognised.
    Level level_from_name(const char *name, bool *ok = nullptr);

    void logf(Level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
} // namespace kv_log

#define KV_LOG_ENABLED(lvl) \
    (static_cast<int>(lvl) <= kv_log::g_level.load(std::memory_order_relaxed))

#define KV_LOG(lvl, ...)                      \
    do                                        \
    {                                         \
        if (KV_LOG_ENABLED(lvl))              \
            kv_log::logf((lvl), __VA_ARGS__); \
    } while (0)

#define KV_ERROR(...) KV_LOG(kv_log::Level::ERROR, __VA_ARGS__)
#define KV_WARN(...) KV_LOG(kv_log::Level::WARN, __VA_ARGS__)
#define KV_INFO(...) KV_LOG(kv_log::Level::INFO, __VA_ARGS__)
#define KV_DEBUG(...) KV_LOG(kv_log::Level::DEBUG, __VA_ARGS__)

#endif // __KV_LOG_H
