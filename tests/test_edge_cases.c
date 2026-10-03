/* Regression cases for filesystem names, arithmetic limits, filtering, and
   real entries and dashboard view membership. */
#define main dupmap_program_main
#include "../src/main.c"
#undef main

#include <assert.h>
#include <fcntl.h>

static void write_file(const char *path, size_t size, unsigned char value) {
    FILE *file = fopen(path, "wb");
    assert(file);
    for (size_t i = 0; i < size; ++i) assert(fputc(value, file) != EOF);
    assert(fclose(file) == 0);
}

static Node *find_child(Node *parent, const char *name) {
    for (size_t i = 0; i < parent->child_count; ++i)
        if (!strcmp(parent->children[i]->name, name)) return parent->children[i];
    return NULL;
}

static void test_real_other_entry_and_small_files(int with_tiny_file) {
    char path[] = "/tmp/dupmap-names-XXXXXX";
    assert(mkdtemp(path));
    char entry[PATH_MAX];
    snprintf(entry, sizeof(entry), "%s/other", path);
    write_file(entry, 3, 'x');
    snprintf(entry, sizeof(entry), "%s/other (2)", path);
    assert(mkdir(entry, 0700) == 0);
    snprintf(entry, sizeof(entry), "%s/large.bin", path);
    write_file(entry, 5000, 'y');
    if (with_tiny_file) {
        snprintf(entry, sizeof(entry), "%s/tiny.txt", path);
        write_file(entry, 7, 'z');
    }

    Node *root = scan_path(path, path, 1);
    assert(root);
    assert(find_child(root, "other"));
    assert(!find_child(root, "other")->is_dir);
    assert(find_child(root, "other")->size == 3);
    assert(!find_child(root, "other (3)"));
    if (with_tiny_file) {
        Node *tiny = find_child(root, "tiny.txt");
        assert(tiny && !tiny->is_dir && tiny->size == 7 && tiny->parent == root);
    }
    assert(root->size == 5003 + (with_tiny_file ? 7 : 0));
    assert(root->file_count == (size_t)(2 + with_tiny_file));
    free_node(root);

    snprintf(entry, sizeof(entry), "%s/other", path); unlink(entry);
    snprintf(entry, sizeof(entry), "%s/other (2)", path); rmdir(entry);
    snprintf(entry, sizeof(entry), "%s/large.bin", path); unlink(entry);
    if (with_tiny_file) {
        snprintf(entry, sizeof(entry), "%s/tiny.txt", path); unlink(entry);
    }
    rmdir(path);
}

static void test_root_symlink_is_not_scanned(void) {
    char path[] = "/tmp/dupmap-link-XXXXXX";
    assert(mkdtemp(path));
    char target[PATH_MAX], link_path[PATH_MAX];
    snprintf(target, sizeof(target), "%s/target", path);
    snprintf(link_path, sizeof(link_path), "%s/link", path);
    assert(mkdir(target, 0700) == 0);
    assert(symlink(target, link_path) == 0);
    assert(scan_path(link_path, link_path, 1) == NULL);
    unlink(link_path);
    rmdir(target);
    rmdir(path);
}

static void test_aggregate_counters_saturate(void) {
    Node *parent = new_node("parent", "/parent", 1);
    Node *first = new_node("first", "/parent/first", 0);
    Node *second = new_node("second", "/parent/second", 0);
    off_t max_size = max_off_t_value();
    first->size = max_size - 5;
    second->size = 10;
    first->file_count = SIZE_MAX - 1;
    second->file_count = 5;
    add_child(parent, first);
    add_child(parent, second);
    assert(parent->size == max_size);
    assert(parent->file_count == SIZE_MAX);
    free_node(parent);
}

static void test_views_filters_and_duplicate_rows(void) {
    Node *parent = new_node("parent", "/parent", 1);
    Node *folder = new_node("empty-folder", "/parent/empty-folder", 1);
    Node *a = new_node("Alpha.txt", "/parent/Alpha.txt", 0);
    Node *b = new_node("alphabet.bin", "/parent/alphabet.bin", 0);
    add_child(parent, folder); add_child(parent, a); add_child(parent, b);
    assert(name_matches("Alpha.txt", "ALP"));
    assert(!name_matches("beta.txt", "ALP"));
    ViewRows rows = {0};
    for (enum View view = VIEW_DASHBOARD; view < VIEW_COUNT; ++view) {
        if (view == VIEW_DUPLICATES) continue;
        build_view_rows(parent, view, "", NULL, 0, SIZE_MAX, &rows);
        assert(rows.count == (view == VIEW_FILES ? 2 : view == VIEW_ALL ? 3 : 1));
        if (view == VIEW_DASHBOARD || view == VIEW_FOLDERS) assert(rows.items[0].node == folder);
    }
    build_view_rows(parent, VIEW_FILES, "ALP", NULL, 0, SIZE_MAX, &rows);
    assert(rows.count == 2 && rows.items[0].node == a);
    build_view_rows(parent, VIEW_ALL, "missing", NULL, 0, SIZE_MAX, &rows);
    assert(rows.count == 0);
    Node *files[] = {a, b};
    DuplicateGroup group = {files, 2, 0};
    build_view_rows(parent, VIEW_DUPLICATES, "", &group, 1, SIZE_MAX, &rows);
    assert(rows.count == 1 && rows.items[0].heading);
    build_view_rows(parent, VIEW_DUPLICATES, "Alpha.txt", &group, 1, 0, &rows);
    assert(rows.count == 3 && rows.items[0].heading && !rows.items[1].heading);
    assert(rows.items[1].node == a && rows.items[2].node == b);
    build_view_rows(parent, VIEW_DUPLICATES, "missing", &group, 1, 0, &rows);
    assert(rows.count == 0);
    char filter[] = "caf\xC3\xA9";
    filter_backspace(filter);
    assert(!strcmp(filter, "caf"));
    assert(!strstr(keyboard_hint(120), "color"));
    free(rows.items); free_node(parent);
}

static void test_sorting_uses_current_mode(void) {
    Node *parent = new_node("parent", "/parent", 1);
    Node *zulu = new_node("Zulu", "/parent/Zulu", 0);
    Node *alpha = new_node("alpha", "/parent/alpha", 0);
    zulu->modified = 100;
    alpha->modified = 50;
    add_child(parent, zulu);
    add_child(parent, alpha);
    int old_sort = sort_mode;
    sort_mode = 1;
    sort_children(parent);
    assert(parent->children[0] == alpha);
    assert(parent->children[1] == zulu);
    sort_mode = 2;
    sort_children(parent);
    assert(parent->children[0] == zulu);
    assert(parent->children[1] == alpha);
    sort_mode = old_sort;
    free_node(parent);
}

static void test_text_clipping_preserves_utf8_sequences(void) {
    char clipped[64];
    clip_text("caf\xC3\xA9" "-document.txt", 8, clipped, sizeof(clipped));
    assert(!strcmp(clipped, "caf\xC3\xA9" "-..."));
    clip_tail("/tmp/caf\xC3\xA9" "-document.txt", 12, clipped, sizeof(clipped));
    assert(!strcmp(clipped, "...ument.txt"));
    clip_text("short", 12, clipped, sizeof(clipped));
    assert(!strcmp(clipped, "short"));
    char small[5];
    clip_text("caf\xC3\xA9", 10, small, sizeof(small));
    assert(!strcmp(small, "caf"));
    clip_tail("caf\xC3\xA9", 10, small, sizeof(small));
    assert(!strcmp(small, "caf"));
    clip_text("name", 0, small, sizeof(small));
    assert(!small[0]);
}

static void test_large_units(void) {
    char size[32];
    uintmax_t petabyte = UINTMAX_C(1) << 50;
    uintmax_t exabyte = UINTMAX_C(1) << 60;
    if ((uintmax_t)max_off_t_value() >= petabyte) {
        format_size((off_t)petabyte, size, sizeof(size));
        assert(!strcmp(size, "1.0 P"));
    }
    if ((uintmax_t)max_off_t_value() >= exabyte) {
        format_size((off_t)exabyte, size, sizeof(size));
        assert(!strcmp(size, "1.0 E"));
    }
}

int main(void) {
    test_real_other_entry_and_small_files(0);
    test_real_other_entry_and_small_files(1);
    test_root_symlink_is_not_scanned();
    test_aggregate_counters_saturate();
    test_views_filters_and_duplicate_rows();
    test_sorting_uses_current_mode();
    test_text_clipping_preserves_utf8_sequences();
    test_large_units();
    puts("edge case tests: all passed");
    return 0;
}
