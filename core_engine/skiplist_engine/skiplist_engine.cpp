#include "skiplist_engine.h"

namespace kv_engine
{
    base_component::Skiplist<string, string> &SkiplistEngine::get_base()
    {
        return data;
    }
}