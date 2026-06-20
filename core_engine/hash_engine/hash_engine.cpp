#include "hash_engine.h"

namespace kv_engine
{
    base_component::HashTable<string, string> &HashEngine::get_base()
    {
        return data;
    }
}