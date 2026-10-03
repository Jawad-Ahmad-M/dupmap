/* Regression cases for filesystem names, arithmetic limits, filtering, and
   tile layouts at the edges of the supported viewport. */
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

static void test_tiny_group_name_does_not_shadow_existing_entry(void) {
    char path[] = "/tmp/dupmap-names-XXXXXX";
    assert(mkdtemp(path));
    char entry[PATH_MAX];
    snprintf(entry, sizeof(entry), "%s/other", path);
    write_file(entry, 3, 'x');
    snprintf(entry, sizeof(entry), "%s/other (2)", path);
    assert(mkdir(entry, 0700) == 0);
    snprintf(entry, sizeof(entry), "%s/large.bin", path);
    write_file(entry, 5000, 'y');

    Node *root = scan_path(path, path, 1);
    assert(root);
    assert(find_child(root, "other"));
    assert(!find_child(root, "other")->is_dir);
    assert(find_child(root, "other (3)"));
    assert(find_child(root, "other (3)")->is_dir);
    assert(find_child(root, "other (3)")->size == 3);
    free_node(root);

    snprintf(entry, sizeof(entry), "%s/other", path); unlink(entry);
    snprintf(entry, sizeof(entry), "%s/other (2)", path); rmdir(entry);
    snprintf(entry, sizeof(entry), "%s/large.bin", path); unlink(entry);
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

static void test_filter_matching_and_wraparound(void) {
    Node *parent = new_node("parent", "/parent", 1);
    add_child(parent, new_node("Alpha.txt", "/parent/Alpha.txt", 0));
    add_child(parent, new_node("beta.txt", "/parent/beta.txt", 0));
    add_child(parent, new_node("middle-one", "/parent/middle-one", 0));
    add_child(parent, new_node("middle-two", "/parent/middle-two", 0));
    add_child(parent, new_node("alphabet.bin", "/parent/alphabet.bin", 0));
    assert(name_matches("Alpha.txt", "ALP"));
    assert(!name_matches("beta.txt", "ALP"));
    assert(next_match(parent, 0, 1, "alp") == 4);
    assert(next_match(parent, 4, 1, "alp") == 0);
    assert(next_match(parent, 0, -1, "alp") == 4);
    assert(name_matches("anything", ""));
    assert(matching_count(parent, "alp") == 2);
    assert(list_window_start(parent, 4, 2, "alp") == 0);
    assert(list_window_start(parent, 4, 1, "alp") == 4);
    free_node(parent);
}

static void test_tile_pulse_never_underlines_tile_fill(void) {
    assert(!(tile_selection_attrs(1, 1, 1) & A_UNDERLINE));
    assert(!(tile_selection_attrs(1, 0, 1) & A_UNDERLINE));
    assert(tile_selection_attrs(1, 0, 0) & A_REVERSE);
    assert(tile_color_pair(1, 1, 3) == 9);
    assert(tile_color_pair(1, 0, 3) == 8);
    assert(tile_color_pair(0, 1, 3) == 3);
}

static void test_monochrome_controls_are_not_advertised(void) {
    ui_color_enabled = 0;
    assert(!strstr(keyboard_hint(80), "color"));
    assert(!strcmp(color_name(), "mono"));
    ui_color_enabled = 1;
    assert(strstr(keyboard_hint(120), "c:color"));
    ui_color_enabled = 0;
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
    assert(child_index(parent, zulu) == 1);
    sort_mode = 2;
    sort_children(parent);
    assert(parent->children[0] == zulu);
    assert(child_index(parent, zulu) == 0);
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
}

static void test_large_tile_sets_remain_inside_view(void) {
    Node *parent = new_node("parent", "/parent", 1);
    for (size_t i = 0; i < 300; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "item-%zu", i);
        Node *child = new_node(name, name, 0);
        child->size = (off_t)(i + 1);
        add_child(parent, child);
    }
    qsort(parent->children, parent->child_count, sizeof(*parent->children), compare_nodes);
    BoxList boxes = {0};
    layout_children(parent, 4, 3, 40, 16, &boxes);
    assert(boxes.count == parent->child_count);
    for (size_t i = 0; i < boxes.count; ++i) {
        Box a = boxes.items[i];
        assert(a.w > 0 && a.h > 0);
        assert(a.x >= 4 && a.y >= 3);
        assert(a.x + a.w <= 44 && a.y + a.h <= 19);
        for (size_t j = i + 1; j < boxes.count; ++j) {
            Box b = boxes.items[j];
            assert(a.node != b.node);
            assert(!(a.x < b.x + b.w && a.x + a.w > b.x &&
                     a.y < b.y + b.h && a.y + a.h > b.y));
        }
    }
    free(boxes.items);
    free_node(parent);
}

static void test_tiny_viewports_and_zero_sized_children(void) {
    Node *parent = new_node("parent", "/parent", 1);
    Node *one = new_node("one", "/parent/one", 0);
    Node *two = new_node("two", "/parent/two", 0);
    Node *zero = new_node("zero", "/parent/zero", 0);
    one->size = 10;
    two->size = 20;
    add_child(parent, one);
    add_child(parent, two);
    add_child(parent, zero);
    BoxList boxes = {0};
    layout_children(parent, 1, 1, 1, 1, &boxes);
    assert(boxes.count <= 1);
    for (size_t i = 0; i < boxes.count; ++i) {
        assert(boxes.items[i].w == 1 && boxes.items[i].h == 1);
        assert(boxes.items[i].node->size > 0);
    }
    free(boxes.items);
    free_node(parent);
}

static void test_layout_dimension_sweep_and_large_units(void) {
    Node *parent = new_node("parent", "/parent", 1);
    for (size_t i = 0; i < 12; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "item-%zu", i);
        Node *child = new_node(name, name, 0);
        child->size = (off_t)(i + 1);
        add_child(parent, child);
    }
    for (int width = 1; width <= 40; width += 3) {
        for (int height = 1; height <= 16; height += 3) {
            BoxList boxes = {0};
            layout_children(parent, 2, 3, width, height, &boxes);
            for (size_t i = 0; i < boxes.count; ++i) {
                Box a = boxes.items[i];
                assert(a.x >= 2 && a.y >= 3);
                assert(a.x + a.w <= 2 + width && a.y + a.h <= 3 + height);
                for (size_t j = i + 1; j < boxes.count; ++j) {
                    Box b = boxes.items[j];
                    assert(!(a.x < b.x + b.w && a.x + a.w > b.x &&
                             a.y < b.y + b.h && a.y + a.h > b.y));
                }
            }
            free(boxes.items);
        }
    }
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
    free_node(parent);
}

int main(void) {
    test_tiny_group_name_does_not_shadow_existing_entry();
    test_root_symlink_is_not_scanned();
    test_aggregate_counters_saturate();
    test_filter_matching_and_wraparound();
    test_tile_pulse_never_underlines_tile_fill();
    test_monochrome_controls_are_not_advertised();
    test_sorting_uses_current_mode();
    test_text_clipping_preserves_utf8_sequences();
    test_large_tile_sets_remain_inside_view();
    test_tiny_viewports_and_zero_sized_children();
    test_layout_dimension_sweep_and_large_units();
    puts("edge case tests: all passed");
    return 0;
}
