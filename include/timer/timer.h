#ifndef __TIMER_H
#define __TIMER_H

#include <chrono>
#include <stdint.h>
#include <functional>
#include <map>
#include <unordered_map>
#include <memory>

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
            Timer() = default;
            ~Timer() = default;
            Timer(int id, Callback cb)
                : timer_id(id), callback(std::move(cb)) {}
            Timer(const Timer &) = delete;
            Timer(Timer &&other) noexcept
                : timer_id(other.timer_id), callback(std::move(other.callback))
            {
                other.timer_id = -1;
            }

            Timer &operator=(const Timer &) = delete;
            Timer &operator=(Timer &&other)
            {
                if (this != &other)
                {
                    timer_id = other.timer_id;
                    callback = std::move(other.callback);
                    other.timer_id = -1;
                }
                return *this;
            }
            int timer_id;
            Callback callback;
        };

        using Timer_t = std::unique_ptr<Timer>;

        using ValueType = std::pair<const TimePoint, Timer_t>;
        using TimerMap = std::multimap<
            TimePoint,
            Timer_t,
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
} // namespace kv_timer

#endif // __TIMER_H