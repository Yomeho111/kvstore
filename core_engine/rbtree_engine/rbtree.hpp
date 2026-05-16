#ifndef RBTREE_HPP
#define RBTREE_HPP
#include <iostream>
#include <string>

#define CLR_RESET "\x1b[0m"
#define CLR_RED "\x1b[31m"
#define CLR_GREEN "\x1b[32m"

template <typename Nodetype>
struct TreeNodeBase
{
    unsigned char color = 'b';
    Nodetype *parent = nullptr;
    Nodetype *left = nullptr;
    Nodetype *right = nullptr;
};

template <typename K, typename V>
struct TreeNode : public TreeNodeBase<TreeNode<K, V>>
{
    K key;
    V value;
    using type = K;
};

template <typename K, typename V>
class RBTree
{
public:
    using NodeType = TreeNode<K, V>;
    RBTree()
    {
        nil = new NodeType();
        root = nil;
    }

    ~RBTree()
    {
        _delAllNode(root);
        delete nil;
        nil = nullptr;
    }

    void insert(NodeType *node)
    {
        NodeType *prev = nil;
        NodeType *cur = root;
        while (cur != nil)
        {
            prev = cur;
            if (cur->key > node->key)
                cur = cur->left;
            else
                cur = cur->right;
        }

        node->parent = prev;
        if (prev == nil)
        {
            root = node;
        }
        else if (prev->key > node->key)
            prev->left = node;
        else
            prev->right = node;

        node->left = nil;
        node->right = nil;
        node->color = 'r';
        _insertFixed(node);
    }

    void delNode(NodeType *node)
    {
        NodeType *real_delete = node;
        NodeType *placement = nullptr;
        unsigned char real_delete_color = node->color;

        if (node->left == nil)
        {
            placement = node->right;
            _transplant(node, placement);
        }
        else if (node->right == nil)
        {
            placement = node->left;
            _transplant(node, placement);
        }
        else
        {
            real_delete = node->right;
            while (real_delete != nil && real_delete->left != nil)
                real_delete = real_delete->left;

            real_delete_color = real_delete->color;
            placement = real_delete->right;

            if (real_delete->parent == node)
            {
                placement->parent = real_delete; // Important: set parent for NIL node
            }
            else
            {
                _transplant(real_delete, placement);
                real_delete->right = node->right;
                real_delete->right->parent = real_delete;
            }
            _transplant(node, real_delete);
            real_delete->left = node->left;
            real_delete->left->parent = real_delete;

            real_delete->color = node->color;
        }
        delete node;
        node = nullptr;
        if (real_delete_color == 'b')
            _deleteFixed(placement);
    }

    void print() const
    {
        if (root == nil)
        {
            std::cout << "(empty tree)" << std::endl;
            return;
        }
        _printTree(root, "", false);
    }

    NodeType *get_node(const typename NodeType::type &key)
    {
        if (root == nil)
        {
            return nullptr;
        }
        NodeType *cur = root;
        while (cur)
        {
            if (cur->key == key)
            {
                return cur;
            }
            else if (cur->key > key)
            {
                cur = cur->left;
            }
            else
            {
                cur = cur->right;
            }
        }
        return cur;
    }

private:
    void _printTree(NodeType *node, const std::string &prefix, bool isLeft) const
    {
        if (node == nil)
            return;

        std::cout << prefix;
        std::cout << (isLeft ? "├── " : "└── ");

        if (node->color == 'r')
            std::cout << CLR_RED << node->key << CLR_RESET << std::endl;
        else
            std::cout << CLR_GREEN << node->key << CLR_RESET << std::endl;

        std::string newPrefix = prefix + (isLeft ? "│   " : "    ");

        bool hasLeft = (node->left != nil);
        bool hasRight = (node->right != nil);

        if (hasLeft || hasRight)
        {
            if (hasLeft)
                _printTree(node->left, newPrefix, hasRight);
            if (hasRight)
                _printTree(node->right, newPrefix, false);
        }
    }

    void _delAllNode(NodeType *cur)
    {
        if (cur == nil)
        {
            return;
        }
        _delAllNode(cur->left);
        _delAllNode(cur->right);
        delete cur;
        cur = nullptr;
        return;
    }

    void _leftRotate(NodeType *cur)
    {
        NodeType *right = cur->right;
        cur->right = right->left;
        if (right->left != nil)
        {
            right->left->parent = cur;
        }

        right->parent = cur->parent;
        if (cur->parent == nil)
        {
            root = right;
        }
        else if (cur->parent->left == cur)
        {
            cur->parent->left = right;
        }
        else
        {
            cur->parent->right = right;
        }

        right->left = cur;
        cur->parent = right;
    }

    void _rightRotate(NodeType *cur)
    {
        NodeType *left = cur->left;
        cur->left = left->right;

        if (left->right != nil)
        {
            left->right->parent = cur;
        }

        left->parent = cur->parent;
        if (cur->parent == nil)
        {
            root = left;
        }
        else if (cur->parent->left == cur)
        {
            cur->parent->left = left;
        }
        else
        {
            cur->parent->right = left;
        }

        left->right = cur;
        cur->parent = left;
    }

    void _insertFixed(NodeType *cur)
    {
        while (cur->parent->color == 'r')
        {
            if (cur->parent == cur->parent->parent->left)
            {
                NodeType *uncle = cur->parent->parent->right;
                if (uncle->color == 'r')
                {
                    cur->parent->color = 'b';
                    uncle->color = 'b';
                    cur->parent->parent->color = 'r';
                    cur = cur->parent->parent;
                }
                else
                {
                    if (cur == cur->parent->right)
                    {
                        cur = cur->parent;
                        _leftRotate(cur);
                    }
                    cur->parent->color = 'b';
                    cur->parent->parent->color = 'r';
                    _rightRotate(cur->parent->parent);
                }
            }
            else
            {
                NodeType *uncle = cur->parent->parent->left;
                if (uncle->color == 'r')
                {
                    cur->parent->color = 'b';
                    uncle->color = 'b';
                    cur->parent->parent->color = 'r';
                    cur = cur->parent->parent;
                }
                else
                {
                    if (cur == cur->parent->left)
                    {
                        cur = cur->parent;
                        _rightRotate(cur);
                    }
                    cur->parent->color = 'b';
                    cur->parent->parent->color = 'r';
                    _leftRotate(cur->parent->parent);
                }
            }
        }
        root->color = 'b';
        return;
    }

    void _transplant(NodeType *dest, NodeType *src)
    {
        if (dest->parent == nil)
        {
            root = src;
        }
        else if (dest->parent->left == dest)
        {
            dest->parent->left = src;
        }
        else
        {
            dest->parent->right = src;
        }
        src->parent = dest->parent;
    }

    void _deleteFixed(NodeType *cur)
    {
        while (cur != root && cur->color == 'b')
        {
            if (cur == cur->parent->left)
            {
                NodeType *sibling = cur->parent->right;
                if (sibling->color == 'r')
                {
                    sibling->color = 'b';
                    cur->parent->color = 'r';
                    _leftRotate(cur->parent);
                    sibling = cur->parent->right;
                }
                if (sibling->left->color == 'b' && sibling->right->color == 'b')
                {
                    sibling->color = 'r';
                    cur = cur->parent;
                }
                else
                {
                    if (sibling->right->color == 'b')
                    {
                        sibling->left->color = 'b';
                        sibling->color = 'r';
                        _rightRotate(sibling);
                        sibling = cur->parent->right;
                    }

                    sibling->color = cur->parent->color;
                    cur->parent->color = 'b';
                    sibling->right->color = 'b';
                    _leftRotate(cur->parent);
                    cur = root;
                }
            }
            else
            {
                NodeType *sibling = cur->parent->left;
                if (sibling->color == 'r')
                {
                    sibling->color = 'b';
                    cur->parent->color = 'r';
                    _rightRotate(cur->parent);
                    sibling = cur->parent->left;
                }

                if (sibling->left->color == 'b' && sibling->right->color == 'b')
                {
                    sibling->color = 'r';
                    cur = cur->parent;
                }
                else
                {
                    if (sibling->left->color == 'b')
                    {
                        sibling->right->color = 'b';
                        sibling->color = 'r';
                        _leftRotate(sibling);
                        sibling = cur->parent->left;
                    }

                    sibling->color = cur->parent->color;
                    cur->parent->color = 'b';
                    sibling->left->color = 'b';
                    _rightRotate(cur->parent);
                    cur = root;
                }
            }
        }
        cur->color = 'b';
    }

    NodeType *root;
    NodeType *nil;
};

#endif // RBTREE_HPP
