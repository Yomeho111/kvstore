#include "timer.h"
#include <mutex>

namespace kv_timer
{
    TimerManager &TimerManager::instance()
    {
        static TimerManager tm;
        return tm;
    }

    int TimerManager::add_event(TimePoint tigger_timestep, Callback callback)
    {
        std::lock_guard lk{lock_};
        int id = next_id++;
        auto it = timers_.emplace(tigger_timestep, Timer{id, std::move(callback)});

        id_to_iter_[id] = it;
        return id;
    }

    int TimerManager::add_event(std::chrono::milliseconds dur, Callback callback)
    {
        TimePoint tigger_timestep = Clock::now() + dur;

        std::lock_guard lk{lock_};
        int id = next_id++;
        auto it = timers_.emplace(tigger_timestep, Timer{id, std::move(callback)});

        id_to_iter_[id] = it;
        return id;
    }

    bool TimerManager::cancel_event(int timer_id)
    {
        std::lock_guard lk{lock_};
        auto it = id_to_iter_.find(timer_id);
        if (it != id_to_iter_.end())
        {
            timers_.erase(it->second);
            id_to_iter_.erase(it);
        }
        return false;
    }

    void TimerManager::handle_expired()
    {
        auto now = Clock::now();

        std::unique_lock lk{lock_};
        while (!timers_.empty())
        {
            auto it = timers_.begin();

            if (it->first > now)
                break;

            int id = it->second.TimerId;
            Callback cb = std::move(it->second.callback);

            timers_.erase(it);
            id_to_iter_.erase(id);

            lk.unlock();
            if (cb)
                cb();
            lk.lock();
        }
    }

    int TimerManager::get_next_timeout_ms()
    {
        std::lock_guard lk{lock_};

        if (timers_.empty())
            return -1;

        auto now = Clock::now();
        auto expire_time = timers_.begin()->first;

        if (now >= expire_time)
            return 0;

        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(expire_time - now);

        return static_cast<int>(diff.count());
    }
}