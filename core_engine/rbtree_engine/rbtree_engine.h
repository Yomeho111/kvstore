#ifndef __RBTREE_ENGINE_H
#define __RBTREE_ENGINE_H

#include <string>

#include "engine_interface.hpp"
#include "rbtree.hpp"
#include "allocator.h"
#include "kv_persistent.h"
#include "memory_utils.h"

namespace kv_engine
{
    // 0 ok, <0 error 1 not exist
    class RbtreeEngine : public EngineInterface<RbtreeEngine>
    {
    public:
        friend class EngineInterface<RbtreeEngine>;
        RbtreeEngine() = default;
        ~RbtreeEngine() = default;

        base_component::RBTree<string, string> &get_base();

    private:
        base_component::RBTree<string, string> rbt;
    };
}

#endif // __RBTREE_ENGINE_H