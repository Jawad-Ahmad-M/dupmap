#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define DUPMAP_VERSION "0.1.0"

typedef struct Node Node;
struct Node {
    char *name;
    char *path;
    Node *parent;
    off_t size;
    int is_dir;
    int inaccessible;
    int is_duplicate;
    time_t modified;
    size_t file_count;
    Node **children;
    size_t child_count;
    size_t child_cap;
};

typedef struct { int x, y, w, h; Node *node; double percent; } Box;
typedef struct { Box *items; size_t count, cap; } BoxList;
typedef struct { Node **files; size_t count; off_t size; } DuplicateGroup;
static int sort_mode = 0; /* 0 size, 1 name, 2 modified */
static int color_mode = 0; /* 0 depth, 1 file type, 2 size heat */
static int ui_active;
static size_t ui_work_count;
static int ui_color_enabled;

static size_t next_character(const char *text, mbstate_t *state, int *cells) {
    wchar_t character;
    size_t bytes = mbrtowc(&character, text, MB_CUR_MAX, state);
    if (bytes == (size_t)-1 || bytes == (size_t)-2) {
        memset(state, 0, sizeof(*state));
        *cells = 1;
        unsigned char lead = (unsigned char)text[0];
        size_t length = lead >= 0xF0 && lead <= 0xF4 ? 4 :
                        (lead >= 0xE0 && lead <= 0xEF ? 3 :
                        (lead >= 0xC2 && lead <= 0xDF ? 2 : 1));
        for (size_t i = 1; i < length; ++i)
            if (!text[i] || ((unsigned char)text[i] & 0xC0) != 0x80) return 1;
        return length;
    }
    if (!bytes) return 0;
    *cells = wcwidth(character);
    if (*cells < 0) *cells = 1;
    return bytes;
}

static int text_width(const char *text) {
    mbstate_t state = {0};
    int width = 0, cells;
    while (*text) {
        size_t bytes = next_character(text, &state, &cells);
        if (!bytes) break;
        width += cells;
        text += bytes;
    }
    return width;
}

static void clip_text(const char *text, int width, char *out, size_t out_size) {
    if (!out_size) return;
    if (width <= 0) { out[0] = '\0'; return; }
    if (text_width(text) <= width) {
        snprintf(out, out_size, "%s", text);
        return;
    }
    int budget = width > 3 ? width - 3 : width;
    size_t copied = 0;
    mbstate_t state = {0};
    int used = 0, cells;
    while (*text) {
        size_t bytes = next_character(text, &state, &cells);
        if (!bytes || used + cells > budget || copied + bytes + 4 > out_size) break;
        memcpy(out + copied, text, bytes);
        copied += bytes;
        used += cells;
        text += bytes;
    }
    if (width > 3 && copied + 4 <= out_size) {
        memcpy(out + copied, "...", 4);
    } else out[copied] = '\0';
}

static void clip_tail(const char *text, int width, char *out, size_t out_size) {
    if (!out_size) return;
    if (text_width(text) <= width) { snprintf(out, out_size, "%s", text); return; }
    if (width <= 3) { clip_text(text, width, out, out_size); return; }
    int budget = width - 3;
    size_t offset = 0;
    int suffix_width = text_width(text);
    mbstate_t state = {0};
    int cells;
    while (text[offset] && suffix_width > budget) {
        size_t bytes = next_character(text + offset, &state, &cells);
        if (!bytes) break;
        offset += bytes;
        suffix_width -= cells;
    }
    snprintf(out, out_size, "...%s", text + offset);
}

static int tile_selection_attrs(int selected, int colors, int pulse) {
    if (!selected) return 0;
    int attrs = A_BOLD;
    if (!colors) attrs |= A_REVERSE;
    (void)pulse;
    return attrs;
}

static int tile_color_pair(int selected, int pulse, int color) {
    return selected ? (pulse ? 9 : 8) : color;
}

static void die(const char *message) {
    if (ui_active) endwin();
    fprintf(stderr, "dupmap: %s\n", message);
    exit(EXIT_FAILURE);
}

static off_t max_off_t_value(void) {
    unsigned shift = (unsigned)(sizeof(uintmax_t) * CHAR_BIT - sizeof(off_t) * CHAR_BIT + 1);
    return (off_t)(UINTMAX_MAX >> shift);
}

static void show_progress(const char *phase, const char *path) {
    if (!ui_active) return;
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (rows < 1 || cols < 1) return;
    char message[PATH_MAX + 80];
    snprintf(message, sizeof(message), "%s: %s  (%zu entries)", phase, path, ui_work_count);
    char clipped[PATH_MAX + 80];
    clip_text(message, cols, clipped, sizeof(clipped));
    mvaddnstr(rows - 1, 0, clipped, (int)strlen(clipped));
    refresh();
}

static char *copy_string(const char *s) {
    char *copy = strdup(s);
    if (!copy) die("out of memory");
    return copy;
}

static char *join_path(const char *parent, const char *name) {
    size_t n = strlen(parent), m = strlen(name), slash = (n && parent[n - 1] != '/') ? 1 : 0;
    char *out = malloc(n + slash + m + 1);
    if (!out) die("out of memory");
    memcpy(out, parent, n);
    if (slash) out[n] = '/';
    memcpy(out + n + slash, name, m + 1);
    return out;
}

static Node *new_node(const char *name, const char *path, int is_dir) {
    Node *node = calloc(1, sizeof(*node));
    if (!node) die("out of memory");
    node->name = copy_string(name);
    node->path = copy_string(path);
    node->is_dir = is_dir;
    node->file_count = is_dir ? 0 : 1;
    return node;
}

static void add_child(Node *parent, Node *child) {
    if (parent->child_count == parent->child_cap) {
        size_t next = parent->child_cap ? parent->child_cap * 2 : 32;
        Node **grown = realloc(parent->children, next * sizeof(*grown));
        if (!grown) die("out of memory");
        parent->children = grown;
        parent->child_cap = next;
    }
    parent->children[parent->child_count++] = child;
    child->parent = parent;
    off_t max_size = max_off_t_value();
    if (child->size > 0 && parent->size > max_size - child->size) parent->size = max_size;
    else parent->size += child->size;
    if (child->file_count > SIZE_MAX - parent->file_count) parent->file_count = SIZE_MAX;
    else parent->file_count += child->file_count;
}

static int compare_nodes(const void *a, const void *b) {
    const Node *left = *(const Node * const *)a, *right = *(const Node * const *)b;
    if (sort_mode == 1) return strcasecmp(left->name, right->name);
    if (sort_mode == 2) {
        if (left->modified < right->modified) return 1;
        if (left->modified > right->modified) return -1;
        return strcasecmp(left->name, right->name);
    }
    if (left->size < right->size) return 1;
    if (left->size > right->size) return -1;
    return strcasecmp(left->name, right->name);
}

static void sort_children(Node *parent) {
    if (parent->child_count)
        qsort(parent->children, parent->child_count, sizeof(*parent->children), compare_nodes);
}

static size_t child_index(Node *parent, Node *child) {
    for (size_t i = 0; i < parent->child_count; ++i)
        if (parent->children[i] == child) return i;
    return 0;
}

static void free_node(Node *node);

static uint64_t file_hash(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned char buffer[8192]; size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
        for (size_t i = 0; i < got; ++i) { hash ^= buffer[i]; hash *= UINT64_C(1099511628211); }
    int failed = ferror(file); fclose(file);
    return failed ? 0 : hash;
}

static int files_equal(const char *left_path, const char *right_path) {
    FILE *left = fopen(left_path, "rb"), *right = fopen(right_path, "rb");
    if (!left || !right) { if (left) fclose(left); if (right) fclose(right); return 0; }
    unsigned char a[8192], b[8192]; size_t na, nb; int equal = 1;
    do {
        na = fread(a, 1, sizeof(a), left); nb = fread(b, 1, sizeof(b), right);
        if (na != nb || memcmp(a, b, na) != 0) { equal = 0; break; }
    } while (na > 0);
    if (ferror(left) || ferror(right)) equal = 0;
    fclose(left); fclose(right); return equal;
}

typedef struct { Node **items; size_t count, cap; } FileList;
static void collect_files(Node *node, FileList *list) {
    ++ui_work_count;
    if ((ui_work_count & 255) == 0) show_progress("Collecting files", node->path);
    if (!node->is_dir) {
        if (list->count == list->cap) {
            size_t next = list->cap ? list->cap * 2 : 64;
            Node **grown = realloc(list->items, next * sizeof(*grown));
            if (!grown) die("out of memory");
            list->items = grown; list->cap = next;
        }
        list->items[list->count++] = node; return;
    }
    for (size_t i = 0; i < node->child_count; ++i) collect_files(node->children[i], list);
}

static size_t find_duplicate_groups(Node *root, DuplicateGroup **out) {
    FileList files = {0}; collect_files(root, &files);
    unsigned char *used = calloc(files.count, 1);
    uint64_t *hashes = malloc(files.count * sizeof(*hashes));
    DuplicateGroup *groups = NULL; size_t count = 0, cap = 0;
    if ((!used || !hashes) && files.count) die("out of memory");
    for (size_t i = 0; i < files.count; ++i) {
        hashes[i] = file_hash(files.items[i]->path);
        ui_work_count = i + 1;
        if ((i & 63) == 0) show_progress("Checking duplicates", files.items[i]->name);
    }
    for (size_t i = 0; i < files.count; ++i) {
        if (used[i]) continue;
        Node **matches = NULL; size_t match_count = 0, match_cap = 0;
        for (size_t j = i; j < files.count; ++j) {
            if (used[j] || files.items[j]->size != files.items[i]->size || hashes[j] != hashes[i]) continue;
            int equal = match_count == 0 || files_equal(files.items[i]->path, files.items[j]->path);
            if (!equal) continue;
            if (match_count == match_cap) {
                size_t next = match_cap ? match_cap * 2 : 4;
                Node **grown = realloc(matches, next * sizeof(*grown));
                if (!grown) die("out of memory");
                matches = grown; match_cap = next;
            }
            matches[match_count++] = files.items[j]; used[j] = 1;
        }
        if (match_count > 1) {
            for (size_t j = 0; j < match_count; ++j) matches[j]->is_duplicate = 1;
            if (count == cap) {
                size_t next = cap ? cap * 2 : 8;
                DuplicateGroup *grown = realloc(groups, next * sizeof(*grown));
                if (!grown) die("out of memory");
                groups = grown; cap = next;
            }
            groups[count++] = (DuplicateGroup){matches, match_count, files.items[i]->size};
        } else free(matches);
    }
    free(hashes); free(used); free(files.items); *out = groups; return count;
}

static void free_duplicate_groups(DuplicateGroup *groups, size_t count) {
    for (size_t i = 0; i < count; ++i) free(groups[i].files);
    free(groups);
}

static Node *scan_path(const char *path, const char *display_name, int is_root) {
    ++ui_work_count;
    if ((ui_work_count & 63) == 0) show_progress("Scanning", path);
    struct stat st;
    if (lstat(path, &st) != 0) return NULL;
    if (S_ISLNK(st.st_mode)) return NULL;
    if (!S_ISDIR(st.st_mode)) {
        Node *file = new_node(display_name, path, 0);
        file->size = st.st_size;
        file->modified = st.st_mtime;
        return file;
    }

    Node *dir = new_node(display_name, path, 1);
    dir->modified = st.st_mtime;
    DIR *handle = opendir(path);
    if (!handle) { dir->inaccessible = 1; return dir; }
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(handle);
        if (!entry) { if (errno) dir->inaccessible = 1; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char *child_path = join_path(path, entry->d_name);
        Node *child = scan_path(child_path, entry->d_name, 0);
        free(child_path);
        if (child) add_child(dir, child);
    }
    if (closedir(handle) != 0) dir->inaccessible = 1;
    /* Keep tiny files from producing unreadable one-cell boxes. Directories
       are never grouped because they must remain independently navigable. */
    const off_t tiny_limit = 4096;
    Node **kept = dir->child_cap ? malloc(dir->child_cap * sizeof(*kept)) : NULL;
    Node **tiny = dir->child_cap ? malloc(dir->child_cap * sizeof(*tiny)) : NULL;
    if (dir->child_cap && (!kept || !tiny)) die("out of memory");
    size_t kept_count = 0, tiny_count = 0;
    for (size_t i = 0; i < dir->child_count; ++i) {
        Node *child = dir->children[i];
        if (!child->is_dir && child->size < tiny_limit) {
            tiny[tiny_count++] = child;
        } else kept[kept_count++] = child;
    }
    free(dir->children);
    dir->children = NULL; dir->child_count = 0; dir->child_cap = 0; dir->size = 0; dir->file_count = 0;
    for (size_t i = 0; i < kept_count; ++i) add_child(dir, kept[i]);
    free(kept);
    if (tiny_count > 0) {
        char other_name[64] = "other";
        unsigned suffix = 2;
        for (;;) {
            int in_use = 0;
            for (size_t i = 0; i < dir->child_count; ++i)
                if (!strcmp(dir->children[i]->name, other_name)) { in_use = 1; break; }
            for (size_t i = 0; i < tiny_count && !in_use; ++i) {
                if (strcmp(tiny[i]->name, other_name)) continue;
                /* A real entry named like a synthetic group must stay visible
                   as itself; move it out of the tiny-file group before trying
                   the next available label. */
                add_child(dir, tiny[i]);
                memmove(&tiny[i], &tiny[i + 1], (tiny_count - i - 1) * sizeof(*tiny));
                --tiny_count;
                in_use = 1;
            }
            if (!in_use) break;
            snprintf(other_name, sizeof(other_name), "other (%u)", suffix++);
        }
        char *other_path = join_path(path, other_name);
        Node *other = new_node(other_name, other_path, 1);
        free(other_path);
        for (size_t i = 0; i < tiny_count; ++i) add_child(other, tiny[i]);
        add_child(dir, other);
    }
    free(tiny);
    sort_children(dir);
    (void)is_root;
    return dir;
}

static void free_node(Node *node) {
    if (!node) return;
    for (size_t i = 0; i < node->child_count; ++i) free_node(node->children[i]);
    free(node->children); free(node->name); free(node->path); free(node);
}

static void add_box(BoxList *list, int x, int y, int w, int h, Node *node, off_t parent_size) {
    if (w < 1 || h < 1) return;
    if (list->count == list->cap) {
        size_t next = list->cap ? list->cap * 2 : 64;
        Box *grown = realloc(list->items, next * sizeof(*grown));
        if (!grown) die("out of memory");
        list->items = grown; list->cap = next;
    }
    double percent = parent_size > 0 ? ((double)node->size * 100.0 / (double)parent_size) : 0.0;
    list->items[list->count++] = (Box){x, y, w, h, node, percent};
}

static double aspect(double area, double short_side) {
    if (short_side <= 0 || area <= 0) return 1e30;
    double long_side = area / short_side;
    return long_side > short_side ? long_side / short_side : short_side / long_side;
}

static size_t layout_scaled(Node *parent, int x, int y, int w, int h, BoxList *boxes) {
    size_t capacity = (size_t)w * (size_t)h, count = 0;
    for (size_t i = 0; i < parent->child_count && count < capacity; ++i)
        if (parent->children[i]->size > 0) ++count;
    if (!count) return 0;

    Node **items = malloc(count * sizeof(*items));
    if (!items) die("out of memory");
    size_t used = 0;
    long double remaining_weight = 0;
    for (size_t i = 0; i < parent->child_count && used < count; ++i) {
        if (parent->children[i]->size <= 0) continue;
        items[used++] = parent->children[i];
        remaining_weight += parent->children[i]->size;
    }

    int columns = 1;
    while (columns < w && (long double)columns * columns * h < (long double)count * w) ++columns;
    int min_columns = (int)((count + (size_t)h - 1) / (size_t)h);
    if (columns < min_columns) columns = min_columns;
    if ((size_t)columns > count) columns = (int)count;

    int remaining_height = h, top = y;
    size_t offset = 0;
    while (offset < count) {
        size_t row_count = count - offset;
        if (row_count > (size_t)columns) row_count = (size_t)columns;
        long double row_weight = 0;
        for (size_t i = 0; i < row_count; ++i) row_weight += items[offset + i]->size;
        size_t rows_after = (count - offset - row_count + (size_t)columns - 1) / (size_t)columns;
        int row_height = rows_after ? (int)((long double)remaining_height * row_weight / remaining_weight + 0.5L)
                                    : remaining_height;
        if (row_height < 1) row_height = 1;
        if (row_height > remaining_height - (int)rows_after) row_height = remaining_height - (int)rows_after;

        int left = x, remaining_width = w;
        long double width_weight = row_weight;
        for (size_t i = 0; i < row_count; ++i) {
            size_t after = row_count - i - 1;
            int tile_width = remaining_width;
            if (after) {
                tile_width = (int)((long double)remaining_width * items[offset + i]->size / width_weight + 0.5L);
                if (tile_width < 1) tile_width = 1;
                if (tile_width > remaining_width - (int)after) tile_width = remaining_width - (int)after;
            }
            add_box(boxes, left, top, tile_width, row_height, items[offset + i], parent->size);
            left += tile_width;
            remaining_width -= tile_width;
            width_weight -= items[offset + i]->size;
        }
        top += row_height;
        remaining_height -= row_height;
        remaining_weight -= row_weight;
        offset += row_count;
    }
    free(items);
    return count;
}

/* Squarified treemap layout. Coordinates are terminal cells. */
static void layout_children(Node *parent, int x, int y, int w, int h, BoxList *boxes) {
    if (!parent->child_count || w < 1 || h < 1) return;
    size_t initial_count = boxes->count, positive_count = 0;
    for (size_t i = 0; i < parent->child_count; ++i)
        if (parent->children[i]->size > 0) ++positive_count;
    if (positive_count > 256) {
        layout_scaled(parent, x, y, w, h, boxes);
        return;
    }
    size_t start = 0;
    int left = x, top = y, width = w, height = h;
    while (start < parent->child_count && width > 0 && height > 0) {
        while (start < parent->child_count && parent->children[start]->size <= 0) ++start;
        if (start == parent->child_count) break;
        int horizontal = width >= height;
        int side = horizontal ? height : width;
        long double remaining_size = 0;
        for (size_t i = start; i < parent->child_count; ++i) remaining_size += parent->children[i]->size;
        if (remaining_size <= 0) break;
        double remaining_area = (double)width * height;
        size_t end = start, best_end = start;
        double best = 1e30;
        double row_area = 0;
        while (end < parent->child_count && parent->children[end]->size > 0) {
            double area = remaining_area * ((double)parent->children[end]->size / (double)remaining_size);
            row_area += area;
            double worst = 0;
            for (size_t i = start; i <= end; ++i) {
                double item_area = remaining_area * ((double)parent->children[i]->size / (double)remaining_size);
                double ratio = aspect(item_area, side);
                if (ratio > worst) worst = ratio;
            }
            if (worst <= best || end == start) { best = worst; best_end = end; ++end; }
            else break;
        }
        int row_extent = horizontal ? width : height;
        if (best_end - start + 1 > (size_t)row_extent) best_end = start + (size_t)row_extent - 1;
        /* A horizontal row spans the available width and consumes height;
           a vertical row spans height and consumes width. */
        long double row_weight = 0;
        for (size_t i = start; i <= best_end; ++i) row_weight += parent->children[i]->size;
        row_area = remaining_area * (double)(row_weight / remaining_size);
        double cross_side = horizontal ? width : height;
        int row_size = (int)(row_area / cross_side + 0.5);
        if (row_size < 1) row_size = 1;
        if (row_size > (horizontal ? height : width)) row_size = horizontal ? height : width;
        size_t next_positive = best_end + 1;
        while (next_positive < parent->child_count && parent->children[next_positive]->size <= 0) ++next_positive;
        int cross_extent = horizontal ? height : width;
        if (next_positive < parent->child_count && cross_extent > 1 && row_size >= cross_extent)
            row_size = cross_extent - 1;
        int cursor = horizontal ? left : top;
        int remaining_length = row_extent;
        long double remaining_row_weight = row_weight;
        for (size_t i = start; i <= best_end; ++i) {
            Node *child = parent->children[i];
            size_t items_after = best_end - i;
            int length = remaining_length;
            if (items_after) {
                length = (int)((long double)remaining_length * child->size / remaining_row_weight + 0.5L);
                if (length < 1) length = 1;
                if (length > remaining_length - (int)items_after) length = remaining_length - (int)items_after;
            }
            if (length < 1) length = 1;
            if (horizontal) { add_box(boxes, cursor, top, length, row_size, child, parent->size); cursor += length; }
            else { add_box(boxes, left, cursor, row_size, length, child, parent->size); cursor += length; }
            remaining_length -= length;
            remaining_row_weight -= child->size;
        }
        if (horizontal) { top += row_size; height -= row_size; }
        else { left += row_size; width -= row_size; }
        start = best_end + 1;
    }
    size_t laid_out = boxes->count - initial_count;
    if (laid_out < positive_count) {
        boxes->count = initial_count;
        layout_scaled(parent, x, y, w, h, boxes);
    }
}

static int depth_color(int depth) { return 1 + (depth % 6); }

static int type_color(const Node *node) {
    if (node->is_dir) return 6;
    const char *dot = strrchr(node->name, '.');
    if (!dot) return 5;
    if (!strcasecmp(dot, ".c") || !strcasecmp(dot, ".h") || !strcasecmp(dot, ".cpp") || !strcasecmp(dot, ".py") || !strcasecmp(dot, ".js")) return 4;
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png") || !strcasecmp(dot, ".gif") || !strcasecmp(dot, ".mp4")) return 2;
    if (!strcasecmp(dot, ".zip") || !strcasecmp(dot, ".gz") || !strcasecmp(dot, ".tar") || !strcasecmp(dot, ".7z")) return 3;
    return 5;
}

static int heat_color(const Node *node) {
    if (node->size >= (off_t)1024 * 1024 * 1024) return 1;
    if (node->size >= (off_t)1024 * 1024) return 3;
    if (node->size >= (off_t)1024) return 5;
    return 6;
}

static int node_color(const Node *node, int depth) {
    if (node->is_duplicate) return 7;
    if (color_mode == 1) return type_color(node);
    if (color_mode == 2) return heat_color(node);
    return depth_color(depth);
}

static void draw_box(const Box *box, int selected, int depth, int pulse) {
    int color = node_color(box->node, depth);
    int x = box->x, y = box->y, w = box->w, h = box->h;
    /* Leave a cell gutter around roomy tiles; preserve every cell in tight layouts. */
    if (w >= 6 && h >= 4) { ++x; ++y; w -= 2; h -= 2; }
    int pair = ui_color_enabled ? COLOR_PAIR(tile_color_pair(selected, pulse, color)) : 0;
    int selection = tile_selection_attrs(selected, ui_color_enabled, pulse);
    attron(pair | selection);
    for (int row = y; row < y + h; ++row) {
        for (int col = x; col < x + w; ++col) mvaddch(row, col, ' ');
    }

    if (selected) attron(selection);
    else if (box->node->is_dir) attron(A_BOLD);
    if (w >= 3 && h >= 1) {
        int start_x = x;
        int label_row = y;
        int max = w; char label[PATH_MAX + 64];
        const char *mark = box->node->is_duplicate ? "* " : "";
        if (box->node->is_dir && w >= 18 && h >= 4)
            snprintf(label, sizeof(label), "%s%s [%zu] %.1f%%", mark, box->node->name, box->node->file_count, box->percent);
        else if (box->node->is_dir) snprintf(label, sizeof(label), "%s%s/ [%zu]", mark, box->node->name, box->node->file_count);
        else if (w >= 18 && h >= 4) snprintf(label, sizeof(label), "%s%s  %.1f%%", mark, box->node->name, box->percent);
        else snprintf(label, sizeof(label), "%s%s", mark, box->node->name);
        char clipped[PATH_MAX + 64];
        clip_text(label, max, clipped, sizeof(clipped));
        mvaddnstr(label_row, start_x, clipped, (int)strlen(clipped));
    }
    attroff(A_REVERSE | A_BOLD | A_UNDERLINE); attroff(pair);
}

static void format_size(off_t value, char *out, size_t length) {
    const char *units[] = {"B", "K", "M", "G", "T", "P", "E"}; double size = (double)value; int unit = 0;
    while (size >= 1024 && unit < 6) { size /= 1024; ++unit; }
    snprintf(out, length, unit ? "%.1f %s" : "%.0f %s", size, units[unit]);
}

static const char *sort_name(void) {
    return sort_mode == 1 ? "name" : (sort_mode == 2 ? "modified" : "size");
}

static const char *color_name(void) {
    if (!ui_color_enabled) return "mono";
    return color_mode == 1 ? "type" : (color_mode == 2 ? "heat" : "depth");
}

static const char *keyboard_hint(int cols) {
    if (cols >= 100)
        return ui_color_enabled
            ? "Arrows select  Enter open  Backspace up  l:list  f:filter  s:sort  c:color  ? help  q:quit"
            : "Arrows select  Enter open  Backspace up  l:list  f:filter  s:sort  ? help  q:quit";
    if (cols >= 76)
        return ui_color_enabled
            ? "Arrows  Enter  Backspace  L list  F filter  S sort  C color  ? help"
            : "Arrows  Enter  Backspace  L list  F filter  S sort  ? help";
    if (cols >= 48)
        return ui_color_enabled
            ? "Arrows move  Enter open  Backspace up  L list  F filter  S sort  C color  ? help"
            : "Arrows move  Enter open  Backspace up  L list  F filter  S sort  ? help";
    return cols >= 32 ? "Arrows move  Enter open  Backspace up  ? help"
                      : "Arrows move  Enter open  ? help";
}

static int load_state(char *last_path, size_t path_length, int *saved_sort, int *saved_color, int *saved_list) {
    const char *base = getenv("XDG_STATE_HOME");
    char state_path[PATH_MAX];
    int length;
    if (base && *base) length = snprintf(state_path, sizeof(state_path), "%s/dupmap/state", base);
    else {
        const char *home = getenv("HOME");
        if (!home || !*home) return 0;
        length = snprintf(state_path, sizeof(state_path), "%s/.config/dupmap/state", home);
    }
    if (length < 0 || (size_t)length >= sizeof(state_path)) return 0;
    FILE *file = fopen(state_path, "r");
    if (!file) return 0;
    if (!fgets(last_path, (int)path_length, file)) { fclose(file); return 0; }
    last_path[strcspn(last_path, "\r\n")] = '\0';
    int parsed = fscanf(file, "%d %d %d", saved_sort, saved_color, saved_list);
    if (parsed != 3) { *saved_sort = 0; *saved_color = 0; *saved_list = 0; }
    fclose(file);
    struct stat st;
    return *last_path && stat(last_path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void save_state(const char *last_path, int saved_sort, int saved_color, int saved_list) {
    const char *base = getenv("XDG_STATE_HOME");
    char state_dir[PATH_MAX], state_path[PATH_MAX];
    int length;
    if (base && *base) length = snprintf(state_dir, sizeof(state_dir), "%s/dupmap", base);
    else {
        const char *home = getenv("HOME");
        if (!home || !*home) return;
        length = snprintf(state_dir, sizeof(state_dir), "%s/.config/dupmap", home);
    }
    if (length < 0 || (size_t)length >= sizeof(state_dir)) return;
    if (mkdir(state_dir, 0700) != 0 && errno != EEXIST) return;
    size_t directory_length = strlen(state_dir);
    if (directory_length + strlen("/state") >= sizeof(state_path)) return;
    memcpy(state_path, state_dir, directory_length);
    memcpy(state_path + directory_length, "/state", strlen("/state") + 1);
    FILE *file = fopen(state_path, "w");
    if (!file) return;
    fprintf(file, "%s\n%d %d %d\n", last_path, saved_sort, saved_color, saved_list);
    fclose(file);
}

static int name_matches(const char *name, const char *query) {
    if (!query[0]) return 1;
    for (const char *start = name; *start; ++start) {
        const char *a = start, *b = query;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
        if (!*b) return 1;
    }
    return 0;
}

static size_t next_match(Node *parent, size_t selected, int direction, const char *query) {
    if (!parent->child_count) return 0;
    size_t index = selected;
    for (size_t step = 0; step < parent->child_count; ++step) {
        if (direction > 0) index = (index + 1) % parent->child_count;
        else index = index ? index - 1 : parent->child_count - 1;
        if (name_matches(parent->children[index]->name, query)) return index;
    }
    return selected;
}

static size_t list_window_start(Node *parent, size_t selected, int rows, const char *query) {
    if (!parent->child_count || rows <= 1) return selected;
    if (selected >= parent->child_count) selected = parent->child_count - 1;
    size_t first = selected;
    int preceding = 0;
    while (first > 0 && preceding < rows - 1) {
        --first;
        if (name_matches(parent->children[first]->name, query)) ++preceding;
    }
    return first;
}

static size_t matching_count(Node *parent, const char *query) {
    size_t count = 0;
    for (size_t i = 0; i < parent->child_count; ++i)
        if (name_matches(parent->children[i]->name, query)) ++count;
    return count;
}

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("Usage: dupmap [options] [path]\n\n"
               "Options:\n"
               "  --dupes [path]  report duplicate files and reclaimable space\n"
               "  -h, --help      show this help\n"
               "  -v, --version   show version\n\n"
               "Interactive keys: arrows select, Enter opens, Backspace goes up,\n"
               "l toggles the complete list view, f filters names, s cycles sorting,\n"
               "c cycles colors, ? shows keyboard help, q quits.\n");
        return EXIT_SUCCESS;
    }
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        puts("dupmap " DUPMAP_VERSION);
        return EXIT_SUCCESS;
    }
    int dupes_mode = argc > 1 && !strcmp(argv[1], "--dupes");
    char cwd[PATH_MAX], saved_path[PATH_MAX]; int saved_list_mode = 0;
    const char *root_path;
    if (dupes_mode) root_path = argc > 2 ? argv[2] : ".";
    else if (argc > 1) root_path = argv[1];
    else if (load_state(saved_path, sizeof(saved_path), &sort_mode, &color_mode, &saved_list_mode)) root_path = saved_path;
    else root_path = getcwd(cwd, sizeof(cwd)) ? cwd : ".";
    char root_check[PATH_MAX];
    size_t root_length = strlen(root_path);
    if (root_length < sizeof(root_check)) {
        memcpy(root_check, root_path, root_length + 1);
        while (root_length > 1 && root_check[root_length - 1] == '/') root_check[--root_length] = '\0';
        struct stat root_stat;
        if (lstat(root_check, &root_stat) == 0 && S_ISLNK(root_stat.st_mode)) {
            fprintf(stderr, "dupmap: cannot read '%s': symbolic links are skipped\n", root_path);
            return EXIT_FAILURE;
        }
    }
    char resolved[PATH_MAX]; if (realpath(root_path, resolved)) root_path = resolved;
    if (!dupes_mode) {
        if (!initscr()) { fprintf(stderr, "dupmap: cannot initialize terminal UI\n"); return EXIT_FAILURE; }
        ui_active = 1; cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
        ui_color_enabled = has_colors() && COLOR_PAIRS > 9 && start_color() != ERR;
        if (ui_color_enabled) {
            int color_setup_ok = 1;
            for (int i = 1; i <= 6; ++i) {
                short foreground = (i == COLOR_YELLOW || i == COLOR_CYAN) ? COLOR_BLACK : COLOR_WHITE;
                if (init_pair(i, foreground, (short)i) == ERR) color_setup_ok = 0;
            }
            if (init_pair(7, COLOR_WHITE, COLOR_RED) == ERR ||
                init_pair(8, COLOR_BLACK, COLOR_WHITE) == ERR ||
                init_pair(9, COLOR_BLACK, COLOR_CYAN) == ERR) color_setup_ok = 0;
            ui_color_enabled = color_setup_ok;
        }
        ui_work_count = 0;
        show_progress("Scanning", root_path);
    }
    Node *root = scan_path(root_path, root_path, 1);
    if (!root) {
        if (ui_active) { endwin(); ui_active = 0; }
        fprintf(stderr, "dupmap: cannot read '%s': %s\n", root_path, strerror(errno));
        return EXIT_FAILURE;
    }
    if (!dupes_mode && !root->is_dir) {
        endwin(); ui_active = 0; free_node(root);
        fprintf(stderr, "dupmap: '%s' is not a directory\n", root_path);
        return EXIT_FAILURE;
    }
    if (dupes_mode) {
        DuplicateGroup *groups = NULL; size_t group_count = find_duplicate_groups(root, &groups);
        printf("Duplicate groups: %zu\n", group_count);
        for (size_t i = 0; i < group_count; ++i) {
            size_t copies = groups[i].count - 1;
            off_t max_size = max_off_t_value();
            off_t reclaimable = groups[i].size > 0 && copies > (size_t)(max_size / groups[i].size)
                                  ? max_size : groups[i].size * (off_t)copies;
            char size_text[32]; format_size(reclaimable, size_text, sizeof(size_text));
            printf("\n%s reclaimable (%zu files):\n", size_text, groups[i].count);
            for (size_t j = 0; j < groups[i].count; ++j) printf("  %s\n", groups[i].files[j]->path);
        }
        free_duplicate_groups(groups, group_count); free_node(root); return EXIT_SUCCESS;
    }

    DuplicateGroup *duplicate_groups = NULL;
    ui_work_count = 0;
    show_progress("Checking duplicates", root_path);
    size_t duplicate_group_count = find_duplicate_groups(root, &duplicate_groups);
    Node *current = root; size_t selected = 0; int list_mode = saved_list_mode; char filter[256] = "";
    int pulse_frames = 0, help_mode = 0;
    BoxList boxes = {0};
    Node *layout_parent = NULL;
    int layout_cols = -1, layout_height = -1, layout_sort = -1;
    for (;;) {
        int rows, cols; getmaxyx(stdscr, rows, cols); erase();
        if (rows < 6 || cols < 16) {
            mvaddnstr(0, 0, "Terminal too small; resize to continue (q quits)", cols > 0 ? cols : 0);
            refresh();
            timeout(-1);
            int key = getch();
            if (key == 'q' || key == 'Q') break;
            continue;
        }
        char size_text[32]; format_size(current->size, size_text, sizeof(size_text));
        int size_width = cols >= 24 ? (int)strlen(size_text) + 2 : 0;
        int path_width = cols - 8 - size_width;
        if (path_width < 1) path_width = 1;
        char short_path[PATH_MAX + 8];
        clip_tail(current->path, path_width, short_path, sizeof(short_path));
        char header[PATH_MAX + 32];
        snprintf(header, sizeof(header), "dupmap  %s", short_path);
        attron(A_BOLD);
        mvaddnstr(0, 0, header, (int)strlen(header));
        attroff(A_BOLD);
        if (cols >= 24) mvprintw(0, cols - (int)strlen(size_text) - 2, "%s", size_text);
        const char *help = keyboard_hint(cols);
        mvaddnstr(1, 0, help, cols);
        int view_y = 2;
        int footer_rows = rows >= 10 ? 2 : 1;
        int view_height = rows - view_y - footer_rows;
        size_t visible_items = 0;
        for (size_t i = 0; i < current->child_count; ++i)
            if (current->children[i]->size > 0) ++visible_items;
        size_t cell_capacity = (size_t)cols * (size_t)view_height;
        int over_capacity = current->child_count > cell_capacity;
        int has_zero_size = visible_items < current->child_count;
        int auto_list = cols < 48 || rows < 12 || over_capacity || has_zero_size;
        int show_list = list_mode || auto_list;
        if (!show_list && (layout_parent != current || layout_cols != cols || layout_height != view_height || layout_sort != sort_mode)) {
            free(boxes.items);
            boxes = (BoxList){0};
            layout_children(current, 0, view_y, cols, view_height, &boxes);
            layout_parent = current;
            layout_cols = cols;
            layout_height = view_height;
            layout_sort = sort_mode;
        }
        Node *selected_node = NULL;
        if (help_mode) {
            const char *help_lines[] = {
                "Keyboard help", "Arrows: move selection  Enter: open folder",
                "Backspace: parent       L: toggle list view", "F: filter names         S: change sort",
                ui_color_enabled ? "C: change colors        ?: close help" : "Colors unavailable        ?: close help",
                "Q: quit"
            };
            int help_count = rows - footer_rows - 2;
            if (help_count > (int)(sizeof(help_lines) / sizeof(help_lines[0]))) help_count = (int)(sizeof(help_lines) / sizeof(help_lines[0]));
            for (int i = 0; i < help_count; ++i) mvaddnstr(2 + i, 0, help_lines[i], cols);
        } else if (show_list) {
            if (filter[0] && (!current->child_count || !name_matches(current->children[selected < current->child_count ? selected : 0]->name, filter))) selected = next_match(current, 0, 1, filter);
            if (selected >= current->child_count && current->child_count) selected = current->child_count - 1;
            int list_start = rows >= 10 ? 3 : view_y;
            int list_rows = rows - footer_rows - list_start;
            if (list_rows < 1) list_rows = 1;
            size_t first = list_window_start(current, selected, list_rows, filter);
            if (rows >= 10) mvprintw(2, 0, "Contents (%zu items)%s:", filter[0] ? matching_count(current, filter) : current->child_count,
                                     auto_list && !list_mode ? (over_capacity ? " (too many items for tiles)" :
                                     (has_zero_size ? " (zero-size items)" : " (compact terminal)")) : "");
            size_t shown = 0;
            for (size_t i = first; i < current->child_count && shown < (size_t)list_rows; ++i) {
                Node *item = current->children[i]; char item_size[32]; format_size(item->size, item_size, sizeof(item_size));
                if (!name_matches(item->name, filter)) continue;
                int line = list_start + (int)shown;
                if (line >= rows - footer_rows) break;
                char entry[PATH_MAX + 64];
                snprintf(entry, sizeof(entry), "%c %s%s  %s%s%s", i == selected ? '>' : ' ', item->is_dir ? "[D] " : "[F] ", item->name,
                         item_size, item->is_duplicate ? "  *" : "", item->inaccessible ? "  [permission denied]" : "");
                if (ui_color_enabled && item->is_duplicate) attron(COLOR_PAIR(7));
                if (i == selected) attron(A_REVERSE | A_BOLD);
                else if (item->is_dir) attron(A_BOLD);
                char clipped_entry[PATH_MAX + 64];
                clip_text(entry, cols, clipped_entry, sizeof(clipped_entry));
                mvaddnstr(line, 0, clipped_entry, (int)strlen(clipped_entry));
                attroff(A_REVERSE | A_BOLD | (ui_color_enabled ? COLOR_PAIR(7) : 0));
                ++shown;
            }
            if (current->child_count && (!filter[0] || name_matches(current->children[selected]->name, filter))) selected_node = current->children[selected];
        } else {
            if (selected >= boxes.count && boxes.count) selected = boxes.count - 1;
            for (size_t i = 0; i < boxes.count; ++i) draw_box(&boxes.items[i], i == selected, 0, pulse_frames > 1);
            if (boxes.count) selected_node = boxes.items[selected].node;
        }
        int status_y = rows - footer_rows;
        if (help_mode) mvaddnstr(status_y, 0, "Help is open  |  ? or Esc closes", cols);
        else if (selected_node) {
            char selected_size[32], status[PATH_MAX + 96]; format_size(selected_node->size, selected_size, sizeof(selected_size));
            size_t position = 0, total = 0;
            for (size_t i = 0; i < current->child_count; ++i) {
                if (!name_matches(current->children[i]->name, filter)) continue;
                ++total;
                if (i == selected) position = total;
            }
            snprintf(status, sizeof(status), "%s  |  %s  |  %zu/%zu%s%s", selected_node->name, selected_size, position, total,
                     selected_node->inaccessible ? "  [permission denied]" : "",
                     selected_node->is_duplicate ? "  [duplicate]" : "");
            char clipped_status[PATH_MAX + 96];
            clip_text(status, cols, clipped_status, sizeof(clipped_status));
            mvaddnstr(status_y, 0, clipped_status, (int)strlen(clipped_status));
        } else if (current->inaccessible) mvaddnstr(status_y, 0, "Cannot read this directory", cols);
        else if (filter[0]) mvaddnstr(status_y, 0, "No items match the current filter", cols);
        else if (current->child_count && !visible_items) mvaddnstr(status_y, 0, "Only zero-byte items; list view is enabled", cols);
        else if (current->child_count) mvaddnstr(status_y, 0, "No visible items in this folder", cols);
        else mvaddnstr(status_y, 0, "This directory is empty", cols);
        if (footer_rows == 2) {
            char footer[128];
            if (help_mode) snprintf(footer, sizeof(footer), "Press ? or Esc to return  |  q quits");
            else snprintf(footer, sizeof(footer), "Sort: %s | Color: %s | [D] folder  [F] file  %s duplicate", sort_name(), color_name(), ui_color_enabled ? "red=" : "*=");
            mvaddnstr(rows - 1, 0, footer, cols);
        }
        refresh();
        timeout(pulse_frames ? 70 : -1);
        int key = getch();
        if (key == ERR) { --pulse_frames; continue; }
        size_t previous_selection = selected;
        if (key == 'q' || key == 'Q') break;
        if (key == '?' || (help_mode && key == 27)) help_mode = !help_mode;
        else if (help_mode) { /* Keep help visible until ? or Escape. */ }
        else if (key == 'l' || key == 'L') { list_mode = !list_mode; }
        else if (key == 'f' || key == 'F') {
            list_mode = 1; echo(); curs_set(1); mvprintw(rows - 1, 0, "Filter (empty clears): "); clrtoeol(); getnstr(filter, sizeof(filter) - 1); noecho(); curs_set(0); selected = 0;
        }
        else if (key == 's' || key == 'S') {
            Node *anchor = selected_node;
            sort_mode = (sort_mode + 1) % 3;
            sort_children(current);
            if (anchor) selected = child_index(current, anchor);
        }
        else if (ui_color_enabled && (key == 'c' || key == 'C')) { color_mode = (color_mode + 1) % 3; }
        else if (show_list && (key == KEY_LEFT || key == KEY_UP)) { if (filter[0]) selected = next_match(current, selected, -1, filter); else if (selected) --selected; }
        else if (show_list && (key == KEY_RIGHT || key == KEY_DOWN)) { if (filter[0]) selected = next_match(current, selected, 1, filter); else if (selected + 1 < current->child_count) ++selected; }
        else if (show_list && (key == '\n' || key == KEY_ENTER) && selected_node && selected_node->is_dir && !selected_node->inaccessible) { current = selected_node; sort_children(current); selected = 0; filter[0] = '\0'; }
        else if (!show_list && (key == KEY_LEFT || key == KEY_UP)) { if (selected) --selected; }
        else if (!show_list && (key == KEY_RIGHT || key == KEY_DOWN)) { if (selected + 1 < boxes.count) ++selected; }
        else if (!show_list && (key == '\n' || key == KEY_ENTER) && selected_node && selected_node->is_dir && !selected_node->inaccessible) { current = selected_node; sort_children(current); selected = 0; filter[0] = '\0'; }
        else if ((key == KEY_BACKSPACE || key == 127 || key == 8) && current != root) {
            Node *child = current;
            current = current->parent;
            selected = child_index(current, child);
            filter[0] = '\0';
        }
        pulse_frames = (!show_list && selected != previous_selection) ? 4 : 0;
    }
    free(boxes.items);
    endwin(); ui_active = 0; save_state(root->path, sort_mode, color_mode, list_mode); free_duplicate_groups(duplicate_groups, duplicate_group_count); free_node(root); return EXIT_SUCCESS;
}
