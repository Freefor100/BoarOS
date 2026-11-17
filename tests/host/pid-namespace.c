#include <kernel/pid.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct space_fixture {
    struct kernel_pid_registry space;
    struct kernel_pid_allocator allocator;
    uint64_t bitmap[KERNEL_PID_BITMAP_WORDS(300)];
};
struct identity_fixture {
    struct kernel_pid id;
    struct kernel_pid_binding bindings[KERNEL_PID_NAMESPACE_MAX_DEPTH + 1];
};
static void put_id(struct kernel_pid *id)
{
    struct kernel_pid_registry *root = id->registry;
    while (root->parent) root = root->parent;
    int final = id->references == 1;
    kernel_pid_put(id);
    if (final) assert(kernel_pid_take_retired(root) == id);
    assert(!root->retired);
}
static void put_space(struct kernel_pid_registry *space)
{
    struct kernel_pid_registry *root = space;
    while (root->parent) root = root->parent;
    kernel_pid_registry_put(space);
    struct kernel_pid_registry *retired;
    while ((retired = kernel_pid_take_retired_namespace(root)))
        assert(!retired->references && !retired->numbers->allocated);
    assert(!root->retired && !root->retired_namespaces);
}
static void init(struct space_fixture *f, struct space_fixture *parent)
{
    assert(!kernel_pid_allocator_init(&f->allocator, f->bitmap, 300));
    assert(!kernel_pid_registry_init(&f->space, &f->allocator, parent ? &parent->space : NULL));
}
static enum kernel_pid_status reserve(struct space_fixture *s, struct identity_fixture *f,
                                      const kernel_pid_t *numbers, size_t count)
{
    return kernel_pid_reserve_numbers(&s->space, &f->id, sizeof(*f), numbers, count);
}
static void numbering(void)
{
    struct space_fixture root = {0}, left = {0}, right = {0}, nested = {0};
    init(&root, NULL); init(&left, &root); init(&right, &root); init(&nested, &left);
    struct identity_fixture a = {0}, b = {0}, c = {0};
    kernel_pid_t an[] = {1, 7, 100}, bn[] = {1, 101}, cn[] = {257, 102};
    assert(!reserve(&nested, &a, an, 3));
    assert(!kernel_pid_find(&root.space, 100));
    assert(!reserve(&right, &b, bn, 2));
    assert(!reserve(&right, &c, cn, 2));
    assert(!kernel_pid_publish_reserved(&a.id));
    assert(!kernel_pid_publish_reserved(&b.id));
    assert(!kernel_pid_publish_reserved(&c.id));
    assert(kernel_pid_find(&nested.space, 1) == &a.id);
    assert(kernel_pid_find(&left.space, 7) == &a.id);
    assert(kernel_pid_find(&root.space, 100) == &a.id);
    assert(kernel_pid_find(&right.space, 1) == &b.id);
    assert(kernel_pid_find(&right.space, 257) == &c.id);
    assert(kernel_pid_number_in(&a.id, &right.space) == 0);
    assert(kernel_pid_number_in(&b.id, &nested.space) == 0);
    assert(kernel_pid_number_in(&a.id, &nested.space) == 1);
    assert(kernel_pid_number_in(&a.id, &left.space) == 7);
    assert(kernel_pid_number_in(&a.id, &root.space) == 100);
    assert(a.id.generation != b.id.generation);
    kernel_pid_get(&a.id);
    struct kernel_pid_member role = {0};
    kernel_pid_attach(&role, &a.id, KERNEL_PID_TID, &role);
    kernel_pid_detach(&role);
    assert(!kernel_pid_find(&root.space, 100));
    struct identity_fixture replacement = {0};
    assert(!reserve(&nested, &replacement, an, 3));
    assert(replacement.id.generation > a.id.generation);
    assert(!kernel_pid_publish_reserved(&replacement.id));
    assert(kernel_pid_find(&nested.space, 1) == &replacement.id);
    assert(a.id.published == 2 && a.id.references == 2);
    /* The old object's historical number remains available to an owner, but
     * lookup cannot return it and no old handle can resolve the replacement. */
    assert(kernel_pid_number_in(&a.id, &nested.space) == 1);
    put_id(&a.id);
    put_id(&a.id);
    put_id(&replacement.id);
    put_id(&b.id); /* Unlink tail of same bucket. */
    assert(kernel_pid_find(&right.space, 257) == &c.id);
    put_id(&c.id);
    put_space(&nested.space); put_space(&left.space);
    put_space(&right.space); put_space(&root.space);
    assert(!root.space.references && !root.space.numbers->allocated);
}
static void mixed_depth_bucket(void)
{
    struct space_fixture root = {0}, child = {0};
    init(&root, NULL); init(&child, &root);
    struct identity_fixture initial = {0}, nested = {0};
    kernel_pid_t first[] = {1}, second[] = {2, 257};
    assert(!reserve(&root, &initial, first, 1));
    assert(!reserve(&child, &nested, second, 2));
    assert(!kernel_pid_publish_reserved(&initial.id));
    assert(!kernel_pid_publish_reserved(&nested.id));
    assert(kernel_pid_find(&root.space, 1) == &initial.id);
    assert(kernel_pid_find(&root.space, 257) == &nested.id);
    assert(kernel_pid_next_published(&root.space, 1) == &nested.id);
    assert(kernel_pid_number_in(&nested.id, &root.space) == 257);
    put_id(&initial.id);
    assert(kernel_pid_find(&root.space, 257) == &nested.id);
    put_id(&nested.id);
    assert(!kernel_pid_take_retired(&root.space));
    put_space(&child.space);
    assert(!root.space.retired_namespaces);
    put_space(&root.space);
}
static void rollback(void)
{
    struct space_fixture root = {0}, child = {0}, leaf = {0};
    init(&root, NULL); init(&child, &root); init(&leaf, &child);
    struct identity_fixture occupied = {0};
    kernel_pid_t numbers[] = {5, 6, 7};
    assert(!reserve(&leaf, &occupied, numbers, 3));
    /* An unpublished reservation conflicts at every depth. Earlier successful
     * allocations must be returned when a later ancestor fails. */
    for (unsigned depth = 0; depth < 3; depth++) {
        struct identity_fixture failed = {0};
        kernel_pid_t request[] = {8, 9, 10};
        request[depth] = numbers[depth];
        assert(reserve(&leaf, &failed, request, 3) == KERNEL_PID_STATUS_EXISTS);
        assert(root.space.numbers->allocated == 1 && child.space.numbers->allocated == 1 &&
               leaf.space.numbers->allocated == 1);
        assert(!failed.id.references && !failed.id.registry);
        struct kernel_pid_binding zero[3] = {0};
        assert(!memcmp(zero, failed.bindings, sizeof(zero)));
        request[depth] = 11;
        assert(!reserve(&leaf, &failed, request, 3));
        put_id(&failed.id);
    }
    struct identity_fixture automatic = {0};
    assert(!reserve(&leaf, &automatic, NULL, 0));
    assert(automatic.bindings[0].number > 0 && automatic.bindings[1].number > 0);
    put_id(&automatic.id);
    struct identity_fixture bad = {0};
    kernel_pid_t invalid[] = {12, 301};
    assert(reserve(&leaf, &bad, invalid, 2) == KERNEL_PID_STATUS_INVALID);
    assert(leaf.space.numbers->allocated == 1 && child.space.numbers->allocated == 1);
    assert(kernel_pid_reserve_numbers(&leaf.space, &bad.id, sizeof(bad.id), NULL, 0)
           == KERNEL_PID_STATUS_INVALID);
    put_id(&occupied.id);
    put_space(&leaf.space); put_space(&child.space);
    put_space(&root.space);
}
static void lifetime_and_disable(void)
{
    struct space_fixture root = {0}, child = {0}, leaf = {0};
    init(&root, NULL); init(&child, &root); init(&leaf, &child);
    struct identity_fixture live = {0}, pending = {0}, failed = {0};
    assert(!reserve(&leaf, &live, NULL, 0));
    assert(!kernel_pid_publish_reserved(&live.id));
    assert(!reserve(&leaf, &pending, NULL, 0));
    kernel_pid_registry_disable(&child.space);
    assert(kernel_pid_publish_reserved(&pending.id) == KERNEL_PID_STATUS_NAMESPACE_DEAD);
    assert(reserve(&leaf, &failed, NULL, 0) == KERNEL_PID_STATUS_NAMESPACE_DEAD);
    struct space_fixture denied = {0};
    assert(!kernel_pid_allocator_init(&denied.allocator, denied.bitmap, 300));
    assert(kernel_pid_registry_init(&denied.space, &denied.allocator, &leaf.space)
           == KERNEL_PID_STATUS_STATE);
    assert(kernel_pid_find(&root.space, live.bindings[0].number) == &live.id);
    put_id(&pending.id);
    put_space(&root.space); put_space(&child.space);
    put_space(&leaf.space);
    assert(root.space.references && child.space.references && leaf.space.references);
    struct kernel_pid_member role = {0};
    kernel_pid_attach(&role, &live.id, KERNEL_PID_TID, &role);
    kernel_pid_detach(&role);
    assert(root.space.references && !root.space.numbers->allocated);
    put_id(&live.id);
    while (kernel_pid_take_retired_namespace(&root.space)) {}
    assert(!root.space.retired && !root.space.retired_namespaces);
    assert(!root.space.references && !child.space.references && !leaf.space.references);
}
static void exhausted_ancestor(void)
{
    struct space_fixture root = {0}, leaf = {0};
    assert(!kernel_pid_allocator_init(&root.allocator, root.bitmap, 1));
    assert(!kernel_pid_registry_init(&root.space, &root.allocator, NULL));
    init(&leaf, &root);
    struct identity_fixture first = {0}, failed = {0};
    assert(!reserve(&root, &first, NULL, 0));
    assert(reserve(&leaf, &failed, NULL, 0) == KERNEL_PID_STATUS_EXHAUSTED);
    assert(!leaf.space.numbers->allocated && root.space.numbers->allocated == 1);
    assert(!failed.id.references && leaf.space.references == 1);
    put_id(&first.id);
    assert(!reserve(&leaf, &failed, NULL, 0));
    put_id(&failed.id);
    put_space(&leaf.space); put_space(&root.space);
}
static void limits(void)
{
    static struct space_fixture spaces[KERNEL_PID_NAMESPACE_MAX_DEPTH + 2];
    for (unsigned i = 0; i <= KERNEL_PID_NAMESPACE_MAX_DEPTH; i++)
        init(&spaces[i], i ? &spaces[i-1] : NULL);
    unsigned max = KERNEL_PID_NAMESPACE_MAX_DEPTH;
    assert(!kernel_pid_allocator_init(&spaces[max+1].allocator, spaces[max+1].bitmap, 300));
    assert(kernel_pid_registry_init(&spaces[max+1].space, &spaces[max+1].allocator,
        &spaces[max].space) == KERNEL_PID_STATUS_EXHAUSTED);
    struct identity_fixture id = {0};
    spaces[0].space.next_generation = KERNEL_PID_GENERATION_MAX;
    assert(!reserve(&spaces[max], &id, NULL, 0));
    assert(!kernel_pid_publish_reserved(&id.id));
    struct identity_fixture overflow = {0};
    assert(reserve(&spaces[max], &overflow, NULL, 0) == KERNEL_PID_STATUS_EXHAUSTED);
    for (unsigned i = 0; i <= max; i++) {
        assert(kernel_pid_find(&spaces[i].space, kernel_pid_number_in(&id.id, &spaces[i].space)) == &id.id);
        put_space(&spaces[i].space);
    }
    put_id(&id.id);
    while (kernel_pid_take_retired_namespace(&spaces[0].space)) {}
    assert(!spaces[0].space.retired && !spaces[0].space.retired_namespaces);
    for (unsigned i = 0; i <= max; i++) assert(!spaces[i].space.references);
}
int main(void)
{
    numbering(); mixed_depth_bucket(); rollback(); lifetime_and_disable(); exhausted_ancestor(); limits();
    puts("pid namespace numbering foundation PASS (not CLONE_NEWPID support)");
    return 0;
}
