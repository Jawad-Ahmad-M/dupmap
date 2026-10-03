/* Exercise scanner and formatting helpers without starting ncurses. */
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

static void test_scan_aggregates_and_direct_entries(void) {
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
    assert(root->file_count == 3);
    assert(child_named(root, "large.bin"));
    assert(child_named(root, "nested"));
    assert(child_named(root, "empty"));
    assert(child_named(root, "link-to-large") == NULL);
    assert(!child_named(root, "other"));
    Node *tiny = child_named(root, "tiny.txt");
    assert(tiny && tiny->size == 3 && tiny->parent == root);
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

static void test_lazy_directories(void) {
    char path[] = "/tmp/dupmap-lazy-XXXXXX";
    assert(mkdtemp(path));
    char *nested_path = join_path(path, "nested");
    char *empty_path = join_path(path, "empty");
    make_dir(nested_path); make_dir(empty_path);
    char *file_path = join_path(nested_path, "payload");
    write_bytes(file_path, 7, 'x');
    ui_work_count = 0;
    Node *root = scan_path(path, path, 0);
    assert(root && root->children_loaded && !root->size_known);
    assert(ui_work_count == 3); /* Root and two immediate entries only. */
    Node *nested = child_named(root, "nested"), *empty = child_named(root, "empty");
    assert(nested && !nested->children_loaded && !nested->child_count);
    char size[32];
    format_node_size(nested, size, sizeof(size)); assert(!strcmp(size, "not calculated"));
    char *late_path = join_path(nested_path, "late");
    write_bytes(late_path, 3, 'y');
    load_children(nested);
    assert(nested->size_known && nested->size == 10 && nested->file_count == 2);
    Node *payload = child_named(nested, "payload");
    size_t scanned = ui_work_count;
    load_children(nested);
    assert(ui_work_count == scanned && child_named(nested, "payload") == payload);
    assert(nested->child_count == 2);
    update_totals(root); assert(!root->size_known && root->size == 10);
    load_tree(root);
    assert(empty->children_loaded && empty->size_known && !empty->size);
    assert(root->size_known && root->size == 10 && root->file_count == 2);
    scanned = ui_work_count;
    load_tree(root);
    assert(ui_work_count == scanned && root->size == 10 && root->file_count == 2);
    free_node(root);
    unlink(file_path); unlink(late_path); rmdir(nested_path); rmdir(empty_path); rmdir(path);
    free(file_path); free(late_path); free(nested_path); free(empty_path);
}

static void test_replaced_lazy_directory(void) {
    char path[] = "/tmp/dupmap-replace-XXXXXX";
    assert(mkdtemp(path));
    char *original = join_path(path, "folder"), *moved = join_path(path, "moved");
    make_dir(original);
    Node *root = scan_path(path, path, 0), *folder = child_named(root, "folder");
    assert(rename(original, moved) == 0);
    make_dir(original);
    load_children(folder);
    assert(folder->inaccessible && folder->children_loaded && !folder->size_known);
    assert(!folder->child_count);
    free_node(root); rmdir(original); rmdir(moved); rmdir(path);
    free(original); free(moved);
}

static void test_formatting(void) {
    char text[32];
    format_size(0, text, sizeof(text)); assert(!strcmp(text, "0 B"));
    format_size(1024, text, sizeof(text)); assert(!strcmp(text, "1.0 K"));
    format_size(1024 * 1024, text, sizeof(text)); assert(!strcmp(text, "1.0 M"));
}

int main(void) {
    test_scan_aggregates_and_direct_entries();
    test_lazy_directories();
    test_replaced_lazy_directory();
    test_formatting();
    puts("dupmap tests: all passed");
    return 0;
}
