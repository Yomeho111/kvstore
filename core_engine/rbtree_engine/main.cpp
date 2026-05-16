#include "rbtree.hpp"

template <>
struct TreeNode<int, int> : public TreeNodeBase<TreeNode<int, int>>
{
    int key;
    int value;
    TreeNode(int k) : TreeNodeBase(), key(k) {}
    TreeNode() : TreeNodeBase() {}
    using type = int;
};

using IntTree = TreeNode<int, int>;

int main()
{
    RBTree<int, int> rbt;
    IntTree *del_node;
    for (int i = 0; i < 10; i++)
    {
        IntTree *node = new IntTree(i);
        if (i == 3)
            del_node = node;
        rbt.insert(node);
    }
    rbt.print();
    rbt.delNode(del_node);
    std::cout << "Get node key " << rbt.get_node(4)->key << "\n";
    std::cout << "++++++++++++++++++++++++++++++++++++\n";
    rbt.print();
}