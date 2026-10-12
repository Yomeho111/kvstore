#ifndef __UTILS_H
#define __UTILS_H

#include <sys/epoll.h>
#include <poll.h>

#ifdef __cplusplus
extern "C"
{
#endif

    uint32_t pollevent_2epoll(short events);

    short epollevent_2poll(uint32_t events);

#ifdef __cplusplus
}
#endif

#endif // __UTILS_H