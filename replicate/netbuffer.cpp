#include "netbuffer.h"

#include <string.h>
#include "allocator.h"

namespace replicate
{
    int Netbuffer::init()
    {
        if (buffer_)
            allocator::kv_free(buffer_);

        buffer_ = (char *)allocator::kv_malloc(capacity_);
        if (!buffer_)
            return -1;

        head_ = 0;
        tail_ = 0;

        return 0;
    }

    int Netbuffer::write_buffer(char *src, size_t size)
    {
        if (!src || size == 0 || !buffer_)
            return -1;
        std::lock_guard lk{mtx_};
        size_t occupied_size = head_ - tail_;
        if (capacity_ - occupied_size < size)
            return -2;

        if (capacity_ - head_ < size)
            _move_to_left();

        memcpy(buffer_ + head_, src, size);

        head_ += size;

        return 0;
    }

    int Netbuffer::get_buffer(char *dst, size_t &size)
    {
        if (!dst || !buffer_)
            return -1;

        std::lock_guard lk{mtx_};
        size_t occupied_size = head_ - tail_;

        size = size > occupied_size ? occupied_size : size;

        memcpy(dst, buffer_ + tail_, size);

        tail_ += size;

        return 0;
    }

    char *Netbuffer::get_buffer(size_t &size)
    {
        if (!buffer_)
            return nullptr;

        size_t occupied_size = head_ - tail_;

        size = size > occupied_size ? occupied_size : size;

        char *sk_buf = buffer_ + tail_;

        tail_ += size;

        return sk_buf;
    }

    size_t Netbuffer::get_size() const
    {
        std::lock_guard lk{mtx_};
        return head_ - tail_;
    }

    Netbuffer::~Netbuffer()
    {
        if (buffer_)
            allocator::kv_free(buffer_);
    }

    void Netbuffer::_move_to_left()
    {
        size_t size = head_ - tail_;

        memmove(buffer_, buffer_ + tail_, size);

        tail_ = 0;
        head_ = size;
    }

} // namespace replicate