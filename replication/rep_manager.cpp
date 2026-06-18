#include "rep_manager.h"

#include <mutex>

namespace replicate
{
    RepManager &RepManager::instance()
    {
        static RepManager manager;
        return manager;
    }

    int RepManager::insert_node(const string *key, const string *value, uint16_t command)
    {
        std::lock_guard lk{lock_};

        int idx = sync_idx_ % MAX_REP_BUFFER_SIZE;
        ring_buff_[idx].command = command;
        ring_buff_[idx].key = key ? *key : string{};
        ring_buff_[idx].value = value ? *value : string{};

        return sync_idx_++;
    }

    const Node &RepManager::operator[](int fd)
    {
        std::lock_guard lk{lock_};
        return ring_buff_[fd % MAX_REP_BUFFER_SIZE];
    }

    int RepManager::get_sync_idx() const
    {
        std::lock_guard lk{lock_};
        return sync_idx_;
    }
}