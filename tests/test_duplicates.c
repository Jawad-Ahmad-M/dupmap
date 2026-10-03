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

static void test_hard_link_reclaimable_space(void) {
    char base[] = "/tmp/dupmap-hardlinks-XXXXXX";
    assert(mkdtemp(base));
    char scan[PATH_MAX], original[PATH_MAX], alias[PATH_MAX], copy[PATH_MAX];
    char outside[PATH_MAX];
    snprintf(scan, sizeof(scan), "%s/scan", base);
    assert(mkdir(scan, 0700) == 0);
    snprintf(original, sizeof(original), "%s/scan/original", base);
    snprintf(alias, sizeof(alias), "%s/scan/alias", base);
    snprintf(copy, sizeof(copy), "%s/scan/copy", base);
    snprintf(outside, sizeof(outside), "%s/outside", base);
    put_file(original, "shared contents\n");
    assert(link(original, alias) == 0);

    for (int scenario = 0; scenario < 3; ++scenario) {
        if (scenario == 1) put_file(copy, "shared contents\n");
        if (scenario == 2) assert(link(copy, outside) == 0);
        Node *root = scan_path(scan, scan, 1);
        assert(root);
        DuplicateGroup *groups = NULL;
        size_t count = find_duplicate_groups(root, &groups);
        assert(count == 1);
        assert(groups[0].count == (size_t)(scenario == 0 ? 2 : 3));
        assert(duplicate_reclaimable_size(&groups[0]) == (scenario == 0 ? 0 : 16));
        for (size_t i = 0; i < groups[0].count; ++i)
            assert(groups[0].files[i]->is_duplicate);
        free_duplicate_groups(groups, count);
        free_node(root);
    }
    /* Both distinct inodes now have links outside the scan; neither can release data. */
    char outside_original[PATH_MAX];
    snprintf(outside_original, sizeof(outside_original), "%s/outside-original", base);
    assert(link(original, outside_original) == 0);
    Node *root = scan_path(scan, scan, 1);
    assert(root);
    DuplicateGroup *groups = NULL;
    size_t count = find_duplicate_groups(root, &groups);
    assert(count == 1 && groups[0].count == 3);
    assert(duplicate_reclaimable_size(&groups[0]) == 0);
    free_duplicate_groups(groups, count);
    free_node(root);
    unlink(outside_original); unlink(outside); unlink(copy); unlink(alias); unlink(original);
    rmdir(scan); rmdir(base);
}

int main(void) {
    test_hard_link_reclaimable_space();
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
        assert(duplicate_reclaimable_size(&groups[i]) == groups[i].size);
        if (groups[i].size == 14) found_tiny = 1;
        if (groups[i].size == 5000) found_large = 1;
    }
    assert(found_tiny && found_large);
    assert(child_named(root, "a.txt") && child_named(root, "a.txt")->is_duplicate);
    assert(child_named(root, "b.txt") && child_named(root, "b.txt")->is_duplicate);
    assert(child_named(root, "different.txt") && !child_named(root, "different.txt")->is_duplicate);
    free_duplicate_groups(groups, count); free_node(root);
    unlink(a); unlink(b); unlink(different); unlink(large_a); unlink(large_b); rmdir(root_path);
    puts("duplicate tests: all passed"); return 0;
}
