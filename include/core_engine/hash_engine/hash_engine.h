#ifndef __HASH_ENGINE_H
#define __HASH_ENGINE_H

#include <string>

#include "engine_interface.hpp"
#include "hash.hpp"

namespace kv_engine
{
    class HashEngine : public EngineInterface<HashEngine>
    {
    public:
        friend class EngineInterface<HashEngine>;
        HashEngine() = default;
        ~HashEngine() = default;

        base_component::HashTable<string, string> &get_base();

    private:
        base_component::HashTable<string, string> data;
    };
} // namespace kv_engine

#endif // __HASH_ENGINE_H