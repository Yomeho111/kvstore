#include "array_engine.h"

namespace kv_engine
{
    base_component::Array<string, string> &ArrayEngine::get_base()
    {
        return data;
    }
} // namespace kv_engine