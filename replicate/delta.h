#ifndef __DELTA_BPF_H
#define __DELTA_BPF_H

struct DeltaEvent
{
    __u32 pid;
    __s32 sequence;
};

#endif // __DELTA_BPF_H