#ifndef __HPC_COROUTINE_H
#define __HPC_COROUTINE_H

#include <ucontext.h>
#include <sys/epoll.h>
#include <poll.h>
#include <unordered_map>
#include <queue>
#include <memory>
#include <future>
#include <functional>
#include <type_traits>

#define EPOLL_EVENTS_SIZE 1024
#define MAX_STACK_SIZE 1024 * 128

namespace hpc_coroutine
{
    class Coroutine;

    using Coroutine_t = std::unique_ptr<Coroutine>;
    enum class CoroutineStatus : uint16_t
    {
        NEW,
        READY,
        WAIT,
        EXIT
    };

    class CoroutineSched
    {
    public:
        static CoroutineSched *get_coroutine_sched(int stack_size = 0);

        void *get_stack() noexcept
        {
            return stack_;
        }

        size_t get_stack_size() noexcept
        {
            return stack_size_;
        }

        ucontext_t *get_ctx() noexcept
        {
            return &main_ctx_;
        }

        // create coroutine -> main functionality
        template <typename F, typename... Args, typename RetType = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>
        std::future<RetType> create_coroutine(F &&f, Args &&...args)
        {
            // type erase
            auto task = std::make_shared<std::packaged_task<RetType()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...));

            auto fut = task->get_future();

            auto co = std::make_unique<Coroutine>(spawned_coroutines_++, this, [task]
                                                  { (*task)(); });

            ready_queue_.push(std::move(co));

            return fut;
        }

        void run();

        int process_epoll();

        bool empty() noexcept
        {
            return ready_queue_.empty() && wait_table_.empty();
        }

        // do schedule if there is no events
        int poll_inner(struct ::pollfd *fds, nfds_t nfds);

    private:
        // singleton
        CoroutineSched(int stack_size) : epfd_(-1), spawned_coroutines_(0), stack_(nullptr), stack_size_(stack_size ? stack_size : MAX_STACK_SIZE), events_(nullptr)
        {
        }
        ~CoroutineSched();

        int init();

        CoroutineSched(const CoroutineSched &) = delete;
        CoroutineSched(CoroutineSched &&) = delete;

        CoroutineSched &operator=(const CoroutineSched &) = delete;
        CoroutineSched &operator=(CoroutineSched &&) = delete;

        int epfd_;
        uint32_t spawned_coroutines_;
        void *stack_;
        Coroutine_t cur_co_;
        size_t stack_size_;
        struct epoll_event *events_;
        ucontext_t main_ctx_;
        std::unordered_map<uint32_t, Coroutine_t> wait_table_; // id: Coroutine
        std::queue<Coroutine_t> ready_queue_;
    };

    class Coroutine
    {
    public:
        Coroutine(uint32_t id, CoroutineSched *sched, std::function<void()> func) : is_ep_(false), fd_(-1), id_(id), status_(CoroutineStatus::NEW), stack_(nullptr), sched_(sched), stack_size_(0), func_(std::move(func)) {}
        ~Coroutine();

        void resume();
        void yield();

        void exec()
        {
            this->func_();
        }

        void set_status(CoroutineStatus new_status) noexcept
        {
            this->status_ = new_status;
        }

        void set_fd(int fd) noexcept
        {
            this->fd_ = fd;
        }

        uint32_t get_id() noexcept
        {
            return id_;
        }

        int get_fd() noexcept
        {
            return fd_;
        }

        void set_ep() noexcept
        {
            is_ep_ = true;
        }

    private:
        Coroutine(const Coroutine &) = delete;
        Coroutine(Coroutine &&) = delete;

        Coroutine &operator=(const Coroutine &) = delete;
        Coroutine &operator=(Coroutine &&) = delete;

        void _save_stack() noexcept;

        void _load_stack() noexcept;

        int init();

        bool is_ep_;
        int fd_;
        uint32_t id_;
        CoroutineStatus status_;
        void *stack_;
        CoroutineSched *sched_;
        size_t stack_size_;
        std::function<void()> func_;
        ucontext_t ctx_;
    };
}

#endif // __HPC_COROUTINE_H