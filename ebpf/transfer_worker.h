#ifndef __TRANSFER_WORKER_H
#define __TRANSFER_WORKER_H

#include <memory>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "lockfree_queue.hpp"
#include "delta.h"

namespace delta
{
    class TransferWorker
    {
        using Slice_t = std::unique_ptr<SliceInfo>;

    public:
        static TransferWorker &instance();

        int submit_slice(Slice_t slice);

    private:
        TransferWorker() = default;
        ~TransferWorker();

        TransferWorker(const TransferWorker &) = delete;
        TransferWorker(TransferWorker &&) = delete;

        TransferWorker &operator=(const TransferWorker &) = delete;
        TransferWorker &operator=(TransferWorker &&) = delete;

        int _init();

        int _transfer_work(Slice_t info);

        std::mutex mtx_;
        std::condition_variable cv_;
        std::atomic<bool> is_running_{false};

        base_component::MpscQueue<Slice_t> que_;
        std::thread thr_;
    };
} // namespace delta

#endif // __TRANSFER_WORKER_H