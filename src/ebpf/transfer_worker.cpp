#include "transfer_worker.h"

#include <stdio.h>
#include <stdlib.h>

#include "cache_pool.h"
#include "tcp_server.h"

namespace delta
{
    TransferWorker &TransferWorker::instance()
    {
        static TransferWorker tw;
        static int ret = tw._init();
        if (ret < 0)
        {
            perror("init TransferWorker error");
            exit(-1);
        }
        return tw;
    }

    int TransferWorker::submit_slice(Slice_t slice)
    {
        que_.enqueue(std::move(slice));

        cv_.notify_one();

        return 0;
    }

    TransferWorker::~TransferWorker()
    {
        is_running_.store(false, std::memory_order_release);

        cv_.notify_all();

        if (thr_.joinable())
            thr_.join();
    }

    int TransferWorker::_init()
    {
        bool expected{false};
        if (!is_running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed))
            return -1;

        thr_ = std::thread(
            [this]
            {
                while (is_running_.load(std::memory_order_acquire))
                {
                    Slice_t info;
                    {
                        std::unique_lock lk{mtx_};
                        cv_.wait(lk, [this, &info]
                                 { return que_.dequeue(info) || !is_running_.load(std::memory_order_acquire); });
                    }

                    if (!info)
                        break;

                    do
                    {
                        if (_transfer_work(std::move(info)) < 0)
                        {
                            perror("Error transfer work");
                            return;
                        }
                    } while (que_.dequeue(info));
                }
            });

        return 0;
    }

    int TransferWorker::_transfer_work(Slice_t info)
    {
        auto &cache_pool = delta::CachePoolManager::instance();
        auto &server = delta::TransferServer::instance();

        __u64 addr_key = info->addr_sport;
        size_t size = info->size;

        if (size == 0 || size > INFO_BUFFER_SIZE)
            return 0;

        char *buffer = info->buf;

        int status_machine = cache_pool.verify_slave(buffer, size, addr_key);
        if (status_machine == 0)
        {
            if (cache_pool.cache_buffer(buffer, size) < 0)
            {
                perror("cache buffer error");
                return -1;
            }
            if (server.broadcast_buffer(buffer, size) < 0)
            {
                perror("broadcast error");
                return -2;
            }
        }
        else if (status_machine == 2)
        {
            struct Node *node = nullptr;
            size_t offset = 0;
            __u64 target_addr_port;

            cache_pool.get_registered_start(addr_key, &node, &offset, &target_addr_port);
            if (target_addr_port == 0 || server.connect(target_addr_port, node, offset) < 0)
            {
                perror("connect error");
                return -3;
            }

            if (node)
                node->fd_start_count--;

            cache_pool.purge_cached_node();
            cache_pool.close_fd(addr_key);
        }

        return 0;
    }
} // namespace delta