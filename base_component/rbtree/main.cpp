#include "rbtree.hpp"
#include <stdio.h>

using IntTree = base_component::TreeNode<int, int>;

int main()
{
    base_component::RBTree<int, int> rbt;
    IntTree *del_node;

    for (int i = 0; i < 10; i++)
    {
        IntTree *node = new IntTree(i, i);
        if (i == 3)
            del_node = node;
        rbt.insert(node);
    }
    // rbt.print();
    rbt.delNode(del_node);
    std::cout << "Get node key " << rbt.get_node(4)->key << "\n";
    std::cout << "++++++++++++++++++++++++++++++++++++\n";
    // rbt.print();
}