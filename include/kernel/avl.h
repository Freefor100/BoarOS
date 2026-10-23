#ifndef BOAROS_KERNEL_AVL_H
#define BOAROS_KERNEL_AVL_H
#include <stdint.h>
/* The owner supplies synchronization and storage lifetime. Equal keys
 * are ordered by node address, independently of the embedding object. */
struct kernel_avl_node {
    struct kernel_avl_node *left,*right;
    uint64_t key;
    int8_t height;
};
static inline int kernel_avl_height(const struct kernel_avl_node *task)
{
    return task ? task->height : 0;
}

static inline void kernel_avl_update(struct kernel_avl_node *task)
{
    int left = kernel_avl_height(task->left);
    int right = kernel_avl_height(task->right);
    task->height = (int8_t)(1 + (left > right ? left : right));
}

static inline struct kernel_avl_node *kernel_avl_rotate_left(struct kernel_avl_node *root)
{
    struct kernel_avl_node *next = root->right;
    root->right = next->left;
    next->left = root;
    kernel_avl_update(root);
    kernel_avl_update(next);
    return next;
}

static inline struct kernel_avl_node *kernel_avl_rotate_right(struct kernel_avl_node *root)
{
    struct kernel_avl_node *next = root->left;
    root->left = next->right;
    next->right = root;
    kernel_avl_update(root);
    kernel_avl_update(next);
    return next;
}

static inline struct kernel_avl_node *kernel_avl_balance(struct kernel_avl_node *root)
{
    kernel_avl_update(root);
    if (kernel_avl_height(root->left) -
            kernel_avl_height(root->right) > 1) {
        if (kernel_avl_height(root->left->left) <
            kernel_avl_height(root->left->right))
            root->left = kernel_avl_rotate_left(root->left);
        return kernel_avl_rotate_right(root);
    }
    if (kernel_avl_height(root->right) -
            kernel_avl_height(root->left) > 1) {
        if (kernel_avl_height(root->right->right) <
            kernel_avl_height(root->right->left))
            root->right = kernel_avl_rotate_right(root->right);
        return kernel_avl_rotate_left(root);
    }
    return root;
}

static inline int kernel_avl_before(const struct kernel_avl_node *a,const struct kernel_avl_node *b)
{ return a->key<b->key || (a->key==b->key && (uintptr_t)a<(uintptr_t)b); }

static inline struct kernel_avl_node *kernel_avl_insert_node(struct kernel_avl_node *root,
                                                struct kernel_avl_node *task)
{
    if (root == 0) return task;
    if (kernel_avl_before(task, root))
        root->left = kernel_avl_insert_node(root->left, task);
    else
        root->right = kernel_avl_insert_node(root->right, task);
    return kernel_avl_balance(root);
}

static inline struct kernel_avl_node *kernel_avl_extract_min(struct kernel_avl_node *root,
                                                struct kernel_avl_node **minimum)
{
    if (root->left == 0) {
        *minimum = root;
        return root->right;
    }
    root->left = kernel_avl_extract_min(root->left, minimum);
    return kernel_avl_balance(root);
}

static inline struct kernel_avl_node *kernel_avl_remove_node(struct kernel_avl_node *root,
                                                const struct kernel_avl_node *task)
{
    if (root == 0) __builtin_trap();
    if (root != task) {
        if (kernel_avl_before(task, root))
            root->left =
                kernel_avl_remove_node(root->left, task);
        else
            root->right =
                kernel_avl_remove_node(root->right, task);
        return kernel_avl_balance(root);
    }
    if (root->left == 0) return root->right;
    if (root->right == 0) return root->left;
    struct kernel_avl_node *next;
    struct kernel_avl_node *right = kernel_avl_extract_min(root->right, &next);
    next->left = root->left;
    next->right = right;
    return kernel_avl_balance(next);
}

static inline struct kernel_avl_node *kernel_avl_first(struct kernel_avl_node *root)
{ while(root&&root->left)root=root->left;return root; }
#endif
