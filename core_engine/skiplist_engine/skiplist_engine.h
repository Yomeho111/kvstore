#ifndef __SKIPLIST_ENGINE_H
#define __SKIPLIST_ENGINE_H

#include <string>

#include "engine_interface.hpp"
#include "skiplist.hpp"

namespace kv_engine
{
    class SkiplistEngine : public EngineInterface<SkiplistEngine>
    {
    public:
        friend class EngineInterface<SkiplistEngine>;
        SkiplistEngine() = default;
        ~SkiplistEngine() = default;

        base_component::Skiplist<string, string> &get_base();

    private:
        base_component::Skiplist<string, string> data;
    };
} // namespace kv_engine

#endif // __SKIPLIST_ENGINE_H