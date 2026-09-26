#define main dupmap_program_main
#include "../src/main.c"
#undef main

#include <assert.h>

static void put_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    fclose(file);
}

static void put_repeated(const char *path, unsigned char value, size_t count) {
    FILE *file = fopen(path, "wb"); assert(file);
    for (size_t i = 0; i < count; ++i) assert(fputc(value, file) != EOF);
    fclose(file);
}

static Node *child_named(Node *parent, const char *name) {
    for (size_t i = 0; i < parent->child_count; ++i)
        if (!strcmp(parent->children[i]->name, name)) return parent->children[i];
    return NULL;
}

int main(void) {
    char root_path[] = "/tmp/dupmap-dupes-XXXXXX";
    assert(mkdtemp(root_path));
    char a[PATH_MAX], b[PATH_MAX], different[PATH_MAX], large_a[PATH_MAX], large_b[PATH_MAX];
    snprintf(a, sizeof(a), "%s/a.txt", root_path);
    snprintf(b, sizeof(b), "%s/b.txt", root_path);
    snprintf(different, sizeof(different), "%s/different.txt", root_path);
    snprintf(large_a, sizeof(large_a), "%s/large-a.bin", root_path);
    snprintf(large_b, sizeof(large_b), "%s/large-b.bin", root_path);
    put_file(a, "same contents\n"); put_file(b, "same contents\n"); put_file(different, "different size\n");
    put_repeated(large_a, 'L', 5000); put_repeated(large_b, 'L', 5000);
    Node *root = scan_path(root_path, root_path, 1); assert(root);
    DuplicateGroup *groups = NULL; size_t count = find_duplicate_groups(root, &groups);
    assert(count == 2);
    int found_tiny = 0, found_large = 0;
    for (size_t i = 0; i < count; ++i) {
        assert(groups[i].count == 2);
        if (groups[i].size == 14) found_tiny = 1;
        if (groups[i].size == 5000) found_large = 1;
    }
    assert(found_tiny && found_large);
    Node *other = child_named(root, "other");
    assert(other && other->is_dir);
    assert(child_named(other, "a.txt") && child_named(other, "a.txt")->is_duplicate);
    assert(child_named(other, "b.txt") && child_named(other, "b.txt")->is_duplicate);
    assert(child_named(other, "different.txt") && !child_named(other, "different.txt")->is_duplicate);
    free_duplicate_groups(groups, count); free_node(root);
    unlink(a); unlink(b); unlink(different); unlink(large_a); unlink(large_b); rmdir(root_path);
    puts("duplicate tests: all passed"); return 0;
}
