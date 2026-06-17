#include "rbtree_engine.h"

namespace kv_engine
{
    base_component::RBTree<string, string> &RbtreeEngine::get_base()
    {
        return rbt;
    }
}