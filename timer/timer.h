#ifndef __TIMER_H
#define __TIMER_H

#include <chrono>
#include <stdint.h>
#include <functional>
#include <map>
#include <unordered_map>

#include "allocator.h"
#include "memory_utils.h"

namespace kv_timer
{
    class TimerManager
    {
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;
        using Callback = std::function<void()>;

        struct Timer
        {
            int TimerId;
            Callback callback;
        };

        using ValueType = std::pair<const TimePoint, Timer>;
        using TimerMap = std::multimap<
            TimePoint,
            Timer,
            std::less<TimePoint>,
            allocator::MyAllocator<ValueType>>;

        using Iterator = TimerMap::iterator;

        using IdMap = std::unordered_map<int, Iterator, std::hash<int>, std::equal_to<int>, allocator::MyAllocator<std::pair<const int, Iterator>>>;

    public:
        static TimerManager &instance();
        int add_event(TimePoint tigger_timestep, Callback callback);
        int add_event(std::chrono::milliseconds dur, Callback callback);

        bool cancel_event(int timer_id);

        void handle_expired();

        int get_next_timeout_ms();

    private:
        TimerManager() = default;
        ~TimerManager() = default;
        TimerManager(const TimerManager &) = delete;
        TimerManager(TimerManager &&) = delete;

        TimerManager &operator=(const TimerManager &) = delete;
        TimerManager &operator=(TimerManager &&) = delete;

        memory::SpinLock lock_;
        int next_id{0};
        TimerMap timers_;
        IdMap id_to_iter_;
    };
}

#endif // __TIMER_H