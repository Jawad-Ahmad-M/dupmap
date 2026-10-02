/* Keep the viewport cases fixed; change the layout implementation to satisfy them. */
#define main dupmap_program_main
#include "../src/main.c"
#undef main

#include <assert.h>

static void assert_layout(Node *root, int width, int height) {
    BoxList boxes = {0};
    layout_children(root, 3, 2, width, height, &boxes);
    size_t visible_cells = width > 0 && height > 0 ? (size_t)width * (size_t)height : 0;
    size_t nonempty = 0;
    for (size_t i = 0; i < root->child_count; ++i)
        if (root->children[i]->size > 0) ++nonempty;
    size_t expected = nonempty < visible_cells ? nonempty : visible_cells;
    assert(boxes.count <= expected);
    if (!visible_cells || !nonempty) assert(boxes.count == 0);
    if (width >= 24 && height >= 8) assert(boxes.count == nonempty);
    for (size_t i = 0; i < boxes.count; ++i) {
        Box a = boxes.items[i];
        assert(a.node->size > 0);
        assert(a.w > 0 && a.h > 0);
        assert(a.x >= 3 && a.y >= 2);
        assert(a.x + a.w <= 3 + width);
        assert(a.y + a.h <= 2 + height);
        assert(a.percent >= 0.0 && a.percent <= 100.0);
        for (size_t j = i + 1; j < boxes.count; ++j) {
            Box b = boxes.items[j];
            assert(a.node != b.node);
            int overlap = a.x < b.x + b.w && a.x + a.w > b.x &&
                          a.y < b.y + b.h && a.y + a.h > b.y;
            assert(!overlap);
        }
    }
    free(boxes.items);
}

int main(void) {
    Node *root = new_node("root", "/root", 1);
    const off_t sizes[] = {1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        char name[32];
        snprintf(name, sizeof(name), "item-%zu", i);
        Node *child = new_node(name, name, 0);
        child->size = sizes[i];
        add_child(root, child);
    }
    add_child(root, new_node("empty-file", "/root/empty-file", 0));
    qsort(root->children, root->child_count, sizeof(*root->children), compare_nodes);

    assert_layout(root, 1, 1);
    assert_layout(root, 2, 8);
    assert_layout(root, 8, 2);
    assert_layout(root, 16, 3);
    assert_layout(root, 24, 8);
    assert_layout(root, 80, 24);
    assert_layout(root, 200, 60);
    assert_layout(root, 0, 24);
    assert_layout(root, 80, 0);
    free_node(root);

    Node *empty = new_node("empty", "/empty", 1);
    assert_layout(empty, 80, 24);
    add_child(empty, new_node("zero", "/empty/zero", 0));
    assert_layout(empty, 80, 24);
    free_node(empty);
    puts("layout dimension tests: all passed");
    return 0;
}
