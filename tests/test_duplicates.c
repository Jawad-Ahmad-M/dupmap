#define main dupmap_program_main
#include "../src/main.c"
#undef main

#include <assert.h>

static void put_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
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
    char a[PATH_MAX], b[PATH_MAX], different[PATH_MAX];
    snprintf(a, sizeof(a), "%s/a.txt", root_path);
    snprintf(b, sizeof(b), "%s/b.txt", root_path);
    snprintf(different, sizeof(different), "%s/different.txt", root_path);
    put_file(a, "same contents\n"); put_file(b, "same contents\n"); put_file(different, "different size\n");
    Node *root = scan_path(root_path, root_path, 1); assert(root);
    DuplicateGroup *groups = NULL; size_t count = find_duplicate_groups(root, &groups);
    assert(count == 1); assert(groups[0].count == 2); assert(groups[0].size == 14);
    assert(child_named(root, "a.txt")->is_duplicate);
    assert(child_named(root, "b.txt")->is_duplicate);
    assert(!child_named(root, "different.txt")->is_duplicate);
    free_duplicate_groups(groups, count); free_node(root);
    unlink(a); unlink(b); unlink(different); rmdir(root_path);
    puts("duplicate tests: all passed"); return 0;
}
