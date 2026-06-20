#ifndef __ARRAY_ENGINE_H
#define __ARRAY_ENGINE_H

#include <string>

#include "engine_interface.hpp"
#include "array.hpp"

namespace kv_engine
{
    class ArrayEngine : public EngineInterface<ArrayEngine>
    {
    public:
        friend class EngineInterface<ArrayEngine>;
        ArrayEngine() = default;
        ~ArrayEngine() = default;

        base_component::Array<string, string> &get_base();

    private:
        base_component::Array<string, string> data;
    };
} // namespace kv_engine

#endif // __ARRAY_ENGINE_H