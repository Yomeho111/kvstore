#ifndef __REP_MANAGER_H
#define __REP_MANAGER_H

#include <string>
#include <stdint.h>
#include "allocator.h"
#include "kv_header.h"
#include "memory_utils.h"

#define MAX_REP_BUFFER_SIZE 64

using string = std::basic_string<
    char,
    std::char_traits<char>,
    allocator::MyAllocator<char>>;

namespace replicate
{

    inline static bool g_replicate{false};

    struct Node
    {
        uint16_t command;
        string key;
        string value;
    };

    class RepManager
    {
    public:
        static RepManager &instance();

        int insert_node(const string *key, const string *value, uint16_t command);

        const Node &operator[](int fd);

        int get_sync_idx() const;

    private:
        RepManager() = default;
        ~RepManager() = default;
        RepManager(const RepManager &) = delete;
        RepManager(RepManager &&) = delete;

        RepManager &operator=(const RepManager &) = delete;
        RepManager &operator=(RepManager &&) = delete;

        mutable memory::SpinLock lock_;
        int sync_idx_{0};
        Node ring_buff_[MAX_REP_BUFFER_SIZE];
    };
} // namespace replicate

#endif // __REP_MANAGER_H