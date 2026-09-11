#ifndef __THREAD_POOL_H
#define __THREAD_POOL_H

#include <future>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>
#include <stddef.h>

#define THREAD_POOL_NUM 2

namespace base_component
{
    class ThreadPool
    {
        using Task = std::function<void()>;

    public:
        static ThreadPool &instance(size_t thread_num = 0)
        {
            static ThreadPool thread_pool{thread_num};
            static int ret = thread_pool._init();
            return thread_pool;
        }

        template <typename F, typename... Args>
        int submit(F &&f, Args &&...args)
        {
            if (stop_)
                return -1;
            auto task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

            {
                std::lock_guard lk{mtx_};
                tasks_.emplace(
                    [p = std::move(task)]() mutable
                    {
                        p();
                    });
            }
            cv_.notify_one();
            return 0;
        }

        void stop()
        {
            stop_.store(true);
            cv_.notify_all();
            for (auto &thr : pool_)
            {
                if (thr.joinable())
                {
                    thr.join();
                }
            }
            pool_.clear();
        }

    private:
        ThreadPool(size_t thread_num) : thread_num_{thread_num}
        {
            start();
        }

        ~ThreadPool()
        {
            stop();
        }

        int _init()
        {
            thread_num_ == 0 ? 2 : thread_num_;
            return 0;
        }

        ThreadPool(const ThreadPool &) = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;

        void start()
        {
            for (size_t i = 0; i < thread_num_; i++)
            {
                pool_.emplace_back(
                    [this]
                    {
                        while (!stop_)
                        {
                            Task task;
                            {
                                std::unique_lock lk{mtx_};
                                cv_.wait(lk, [this]
                                         { return stop_ || !tasks_.empty(); });
                                if (tasks_.empty())
                                    return;
                                task = std::move(tasks_.front());
                                tasks_.pop();
                            }
                            task();
                        }
                    });
            }
        }

        std::mutex mtx_;
        std::condition_variable cv_;
        std::atomic_bool stop_;
        size_t thread_num_;
        std::vector<std::thread> pool_;
        std::queue<Task> tasks_;
    };
} // namespace base_component

#endif // __THREAD_POOL_H