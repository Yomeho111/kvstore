#include "hpc_coroutine.h"
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "utils.h"
#include "kv_log.h"

#include "allocator.h"
#include "timer.h"

#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#define KVSTORE_HAS_VALGRIND_STACK_API 1
#endif

namespace hpc_coroutine
{
    void _exec(void *lt)
    {
        Coroutine *co = (Coroutine *)lt;
        co->exec();
        co->set_status(CoroutineStatus::EXIT);
        co->yield();
    }

    CoroutineSched::~CoroutineSched()
    {
        if (epfd_ >= 0)
        {
            close(epfd_);
            epfd_ = -1;
        }

        if (events_ != nullptr)
        {
            allocator::kv_free(events_);
            events_ = nullptr;
        }
    }

    int CoroutineSched::init()
    {
        // initialize epoll
        epfd_ = epoll_create(1024);
        if (epfd_ < 0)
        {
            KV_ERROR("Error epoll_create");
            return -1;
        }

        events_ = (struct epoll_event *)allocator::kv_malloc(sizeof(struct epoll_event) * EPOLL_EVENTS_SIZE);
        if (events_ == nullptr) [[unlikely]]
        {
            KV_ERROR("Error allocating events_");
            return -1;
        }

        return 0;
    }

    CoroutineSched *CoroutineSched::get_coroutine_sched()
    {
        static thread_local CoroutineSched sched;
        static thread_local bool is_init{false};
        if (!is_init)
        {
            sched.init();
            is_init = true;
        }
        return &sched;
    }

    void CoroutineSched::run()
    {
        while (!g_shutdown && !empty())
        {
            // first process ready
            while (!g_shutdown && !ready_queue_.empty())
            {
                cur_co_ = std::move(ready_queue_.front());
                ready_queue_.pop();

                cur_co_->resume();
            }

            if (g_shutdown)
                break;

            // process the epoll
            if (process_epoll() < 0)
                break;
        }
    }

    int CoroutineSched::process_epoll()
    {
        auto &timer_m = kv_timer::TimerManager::instance();
        int wait_time = timer_m.get_next_timeout_ms();
        int nready = epoll_wait(epfd_, events_, EPOLL_EVENTS_SIZE, wait_time);
        if (nready < 0)
        {
            // epoll_wait is never auto-restarted; a delivered signal just wakes us up.
            if (errno != EINTR)
            {
                KV_ERROR("Error epoll_wait");
                return -1;
            }
            nready = 0;
        }

        timer_m.handle_expired();

        // process ready fd
        for (int i = 0; i < nready; i++)
        {
            uint32_t co_id = events_[i].data.u32;
            auto it = wait_table_.find(co_id);
            if (it == wait_table_.end())
                continue;
            Coroutine_t co = std::move(it->second);
            wait_table_.erase(it);
            co->set_status(CoroutineStatus::READY);
            ready_queue_.push(std::move(co));
        }
        return nready;
    }

    int CoroutineSched::poll_inner(struct ::pollfd *fds, ::nfds_t nfds)
    {
        int nready = ::poll(fds, nfds, 0);
        if (nready == 0)
        {
            // we need to register the fd into epoll, and also put the current co to wait_table_
            uint32_t co_id = cur_co_->get_id();
            cur_co_->set_fd(fds->fd);

            struct ::epoll_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.data.u32 = co_id;
            ev.events = pollevent_2epoll(fds->events);

            if (epoll_ctl(epfd_, EPOLL_CTL_ADD, cur_co_->get_fd(), &ev) < 0)
            {
                if (errno == EEXIST)
                    epoll_ctl(epfd_, EPOLL_CTL_MOD, fds->fd, &ev);
                else
                {
                    KV_ERROR("epoll_ctl ADD");
                    return -1;
                }
            }
            else
                cur_co_->set_ep();
            cur_co_->set_status(CoroutineStatus::WAIT);
            wait_table_[co_id] = std::move(cur_co_);
            cur_co_ = nullptr;
            wait_table_[co_id]->yield();
        }
        else if (nready < 0) [[unlikely]]
        {
            KV_ERROR("Error poll_inner");
            return -1;
        }

        return 0;
    }

    void CoroutineSched::co_sleep(int ms)
    {
        if (ms <= 0 || cur_co_ == nullptr)
            return;

        uint32_t co_id = cur_co_->get_id();
        cur_co_->set_status(CoroutineStatus::WAIT);
        sleep_table_[co_id] = std::move(cur_co_);
        cur_co_ = nullptr;

        // The timer callback runs from handle_expired() on the scheduler's main
        // stack, so it is safe to move the coroutine back to the ready queue.
        kv_timer::TimerManager::instance().add_event(
            std::chrono::milliseconds(ms),
            [this, co_id]()
            {
                auto it = sleep_table_.find(co_id);
                if (it == sleep_table_.end())
                    return;
                Coroutine_t co = std::move(it->second);
                sleep_table_.erase(it);
                co->set_status(CoroutineStatus::READY);
                ready_queue_.push(std::move(co));
            });

        sleep_table_[co_id]->yield();
    }

    Coroutine::~Coroutine()
    {
#ifdef KVSTORE_HAS_VALGRIND_STACK_API
        if (valgrind_stack_id_ != 0)
            VALGRIND_STACK_DEREGISTER(valgrind_stack_id_);
#endif
        if (stack_ != nullptr)
        {
            allocator::kv_free(stack_);
            stack_ = nullptr;
        }
    }

    int Coroutine::init()
    {
        stack_ = allocator::kv_malloc(MAX_STACK_SIZE);
        if (stack_ == nullptr)
            return -1;

#ifdef KVSTORE_HAS_VALGRIND_STACK_API
        valgrind_stack_id_ = VALGRIND_STACK_REGISTER(
            stack_, static_cast<char *>(stack_) + MAX_STACK_SIZE);
#endif

        getcontext(&ctx_);
        ctx_.uc_stack.ss_sp = stack_;
        ctx_.uc_stack.ss_size = MAX_STACK_SIZE;
        ctx_.uc_link = sched_->get_ctx();

        makecontext(&ctx_, (void (*)(void))_exec, 1, this);
        status_ = CoroutineStatus::READY;
        return 0;
    }

    void Coroutine::resume()
    {
        if (status_ == CoroutineStatus::NEW)
        {
            if (init() < 0)
            {
                status_ = CoroutineStatus::EXIT;
                return;
            }
        }
        swapcontext(sched_->get_ctx(), &ctx_);
    }

    void Coroutine::yield()
    {
        swapcontext(&ctx_, sched_->get_ctx());
    }
} // namespace hpc_coroutine