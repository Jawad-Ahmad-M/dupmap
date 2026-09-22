/* Include the implementation so the V1 core can be tested without starting
   ncurses. This is intentionally temporary test architecture for V1; the
   scanner/layout code can be split into a library when the UI grows. */
#define main dupmap_program_main
#include "../src/main.c"
#undef main

#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>

static void make_dir(const char *path) {
    assert(mkdir(path, 0700) == 0);
}

static void write_bytes(const char *path, size_t count, unsigned char value) {
    FILE *file = fopen(path, "wb");
    assert(file);
    for (size_t i = 0; i < count; ++i) assert(fputc(value, file) != EOF);
    assert(fclose(file) == 0);
}

static Node *child_named(Node *parent, const char *name) {
    for (size_t i = 0; i < parent->child_count; ++i)
        if (!strcmp(parent->children[i]->name, name)) return parent->children[i];
    return NULL;
}

static void test_scan_aggregates_and_groups(void) {
    char root_path[] = "/tmp/dupmap-test-XXXXXX";
    assert(mkdtemp(root_path));
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/large.bin", root_path);
    write_bytes(path, 5000, 0xA);
    snprintf(path, sizeof(path), "%s/tiny.txt", root_path);
    write_bytes(path, 3, 0xB);
    snprintf(path, sizeof(path), "%s/nested", root_path);
    make_dir(path);
    char *nested_path = join_path(path, "data.bin");
    write_bytes(nested_path, 6000, 0xC);
    free(nested_path);
    snprintf(path, sizeof(path), "%s/empty", root_path);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/link-to-large", root_path);
    assert(symlink("large.bin", path) == 0);

    Node *root = scan_path(root_path, root_path, 1);
    assert(root);
    assert(root->is_dir);
    assert(root->size == 11003);
    assert(child_named(root, "large.bin"));
    assert(child_named(root, "nested"));
    assert(child_named(root, "empty"));
    assert(child_named(root, "link-to-large") == NULL);
    Node *other = child_named(root, "other");
    assert(other && other->is_dir && other->size == 3);
    assert(child_named(other, "tiny.txt"));
    assert(child_named(other, "tiny.txt")->parent == other);
    Node *nested = child_named(root, "nested");
    assert(nested->size == 6000);
    assert(child_named(nested, "data.bin"));
    assert(child_named(nested, "data.bin")->parent == nested);
    assert(nested->parent == root);

    free_node(root);
    snprintf(path, sizeof(path), "%s/link-to-large", root_path); unlink(path);
    snprintf(path, sizeof(path), "%s/empty", root_path); rmdir(path);
    snprintf(path, sizeof(path), "%s/nested/data.bin", root_path); unlink(path);
    snprintf(path, sizeof(path), "%s/nested", root_path); rmdir(path);
    snprintf(path, sizeof(path), "%s/large.bin", root_path); unlink(path);
    snprintf(path, sizeof(path), "%s/tiny.txt", root_path); unlink(path);
    rmdir(root_path);
}

static void test_layout_stays_inside_view(void) {
    char root_path[] = "/tmp/dupmap-layout-XXXXXX";
    assert(mkdtemp(root_path));
    char path[PATH_MAX];
    for (int i = 0; i < 8; ++i) {
        snprintf(path, sizeof(path), "%s/file-%d.bin", root_path, i);
        write_bytes(path, (size_t)(5000 + i * 1000), (unsigned char)i);
    }
    Node *root = scan_path(root_path, root_path, 1);
    assert(root);
    BoxList boxes = {0};
    const int x = 2, y = 3, width = 80, height = 24;
    layout_children(root, x, y, width, height, &boxes);
    assert(boxes.count == root->child_count);
    for (size_t i = 0; i < boxes.count; ++i) {
        Box a = boxes.items[i];
        assert(a.x >= x && a.y >= y);
        assert(a.x + a.w <= x + width && a.y + a.h <= y + height);
        assert(a.w > 0 && a.h > 0);
        for (size_t j = i + 1; j < boxes.count; ++j) {
            Box b = boxes.items[j];
            int overlap = a.x < b.x + b.w && a.x + a.w > b.x &&
                          a.y < b.y + b.h && a.y + a.h > b.y;
            assert(!overlap);
        }
    }
    free(boxes.items);
    free_node(root);
    for (int i = 0; i < 8; ++i) { snprintf(path, sizeof(path), "%s/file-%d.bin", root_path, i); unlink(path); }
    rmdir(root_path);
}

static void test_formatting(void) {
    char text[32];
    format_size(0, text, sizeof(text)); assert(!strcmp(text, "0 B"));
    format_size(1024, text, sizeof(text)); assert(!strcmp(text, "1.0 K"));
    format_size(1024 * 1024, text, sizeof(text)); assert(!strcmp(text, "1.0 M"));
}

int main(void) {
    test_scan_aggregates_and_groups();
    test_layout_stays_inside_view();
    test_formatting();
    puts("dupmap tests: all passed");
    return 0;
}
