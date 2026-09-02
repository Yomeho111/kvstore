#ifndef __NETBUFFER_H
#define __NETBUFFER_H

#include <stddef.h>
#include <mutex>

namespace replicate
{
    class Netbuffer
    {
    public:
        Netbuffer(size_t capacity) : capacity_(capacity), buffer_(nullptr) {}

        int init();

        int write_buffer(char *src, size_t size);

        int get_buffer(char *dst, size_t &size);

        char *get_buffer(size_t &size);

        size_t get_size() const;

        ~Netbuffer();

    private:
        Netbuffer(const Netbuffer &) = delete;
        Netbuffer(Netbuffer &&) = delete;

        Netbuffer &operator=(const Netbuffer &) = delete;
        Netbuffer &operator=(Netbuffer &&) = delete;

        void _move_to_left();

        size_t tail_{0};
        size_t head_{0};
        size_t capacity_;
        char *buffer_;

        mutable std::mutex mtx_;
    };
} // namespace replicate

#endif // __NETBUFFER_H