#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct Node Node;
struct Node {
    char *name;
    char *path;
    Node *parent;
    off_t size;
    int is_dir;
    int inaccessible;
    Node **children;
    size_t child_count;
    size_t child_cap;
};

typedef struct { int x, y, w, h; Node *node; } Box;
typedef struct { Box *items; size_t count, cap; } BoxList;

static void die(const char *message) { endwin(); fprintf(stderr, "dupmap: %s\n", message); exit(EXIT_FAILURE); }

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
    parent->size += child->size;
}

static int compare_nodes(const void *a, const void *b) {
    const Node *left = *(const Node * const *)a, *right = *(const Node * const *)b;
    if (left->size < right->size) return 1;
    if (left->size > right->size) return -1;
    return strcasecmp(left->name, right->name);
}

static void free_node(Node *node);

static Node *scan_path(const char *path, const char *display_name, int is_root) {
    struct stat st;
    if (lstat(path, &st) != 0) return NULL;
    if (S_ISLNK(st.st_mode)) return NULL;
    if (!S_ISDIR(st.st_mode)) {
        Node *file = new_node(display_name, path, 0);
        file->size = st.st_size;
        return file;
    }

    Node *dir = new_node(display_name, path, 1);
    DIR *handle = opendir(path);
    if (!handle) { dir->inaccessible = 1; return dir; }
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char *child_path = join_path(path, entry->d_name);
        Node *child = scan_path(child_path, entry->d_name, 0);
        free(child_path);
        if (child) add_child(dir, child);
    }
    closedir(handle);
    /* Keep tiny files from producing unreadable one-cell boxes. Directories
       are never grouped because they must remain independently navigable. */
    const off_t tiny_limit = 4096;
    off_t other_size = 0;
    Node **kept = malloc(dir->child_cap * sizeof(*kept));
    if (!kept) die("out of memory");
    size_t kept_count = 0;
    for (size_t i = 0; i < dir->child_count; ++i) {
        Node *child = dir->children[i];
        if (!child->is_dir && child->size < tiny_limit) {
            other_size += child->size;
            free_node(child);
        } else kept[kept_count++] = child;
    }
    free(dir->children);
    dir->children = NULL; dir->child_count = 0; dir->child_cap = 0; dir->size = 0;
    for (size_t i = 0; i < kept_count; ++i) add_child(dir, kept[i]);
    free(kept);
    if (other_size > 0) {
        Node *other = new_node("other", path, 0);
        other->size = other_size;
        add_child(dir, other);
    }
    if (dir->child_count) qsort(dir->children, dir->child_count, sizeof(*dir->children), compare_nodes);
    (void)is_root;
    return dir;
}

static void free_node(Node *node) {
    if (!node) return;
    for (size_t i = 0; i < node->child_count; ++i) free_node(node->children[i]);
    free(node->children); free(node->name); free(node->path); free(node);
}

static void add_box(BoxList *list, int x, int y, int w, int h, Node *node) {
    if (w < 1 || h < 1) return;
    if (list->count == list->cap) {
        size_t next = list->cap ? list->cap * 2 : 64;
        Box *grown = realloc(list->items, next * sizeof(*grown));
        if (!grown) die("out of memory");
        list->items = grown; list->cap = next;
    }
    list->items[list->count++] = (Box){x, y, w, h, node};
}

static double aspect(double area, double short_side) {
    if (short_side <= 0 || area <= 0) return 1e30;
    double long_side = area / short_side;
    return long_side > short_side ? long_side / short_side : short_side / long_side;
}

/* Squarified treemap layout. Coordinates are terminal cells. */
static void layout_children(Node *parent, int x, int y, int w, int h, BoxList *boxes) {
    if (!parent->child_count || w < 1 || h < 1) return;
    size_t start = 0;
    int left = x, top = y, width = w, height = h;
    while (start < parent->child_count && width > 0 && height > 0) {
        int horizontal = width >= height;
        int side = horizontal ? height : width;
        long double remaining_size = 0;
        for (size_t i = start; i < parent->child_count; ++i) remaining_size += parent->children[i]->size;
        if (remaining_size <= 0) break;
        double remaining_area = (double)width * height;
        size_t end = start, best_end = start;
        double best = 1e30;
        double row_area = 0;
        while (end < parent->child_count) {
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
        /* A horizontal row spans the available width and consumes height;
           a vertical row spans height and consumes width. */
        double cross_side = horizontal ? width : height;
        int row_size = (int)(row_area / cross_side + 0.5);
        if (row_size < 1) row_size = 1;
        if (row_size > (horizontal ? height : width)) row_size = horizontal ? height : width;
        int cursor = horizontal ? left : top;
        double actual_row = 0;
        for (size_t i = start; i <= best_end; ++i) actual_row += (double)parent->children[i]->size / (double)remaining_size * remaining_area;
        for (size_t i = start; i <= best_end; ++i) {
            Node *child = parent->children[i];
            int length = (i == best_end) ? (horizontal ? left + width - cursor : top + height - cursor)
                                         : (int)((actual_row * ((double)child->size / (double)(remaining_size))) / row_size + 0.5);
            if (length < 1) length = 1;
            if (horizontal) { add_box(boxes, cursor, top, length, row_size, child); cursor += length; }
            else { add_box(boxes, left, cursor, row_size, length, child); cursor += length; }
        }
        if (horizontal) { top += row_size; height -= row_size; }
        else { left += row_size; width -= row_size; }
        start = best_end + 1;
    }
}

static int depth_color(int depth) { return 1 + (depth % 6); }

static void draw_box(const Box *box, int selected, int depth) {
    int color = depth_color(depth);
    attron(COLOR_PAIR(color));
    for (int row = box->y; row < box->y + box->h; ++row) {
        for (int col = box->x; col < box->x + box->w; ++col) mvaddch(row, col, ' ' | COLOR_PAIR(color));
    }
    if (selected) attron(A_REVERSE | A_BOLD);
    if (box->w >= 3 && box->h >= 1) {
        int max = box->w - 1; char label[256];
        snprintf(label, sizeof(label), "%s", box->node->name);
        if ((int)strlen(label) > max) { if (max > 3) { label[max - 3] = '.'; label[max - 2] = '.'; label[max - 1] = '.'; label[max] = '\0'; } else label[max] = '\0'; }
        mvaddnstr(box->y, box->x, label, max);
    }
    attroff(A_REVERSE | A_BOLD); attroff(COLOR_PAIR(color));
}

static void format_size(off_t value, char *out, size_t length) {
    const char *units[] = {"B", "K", "M", "G", "T"}; double size = (double)value; int unit = 0;
    while (size >= 1024 && unit < 4) { size /= 1024; ++unit; }
    snprintf(out, length, unit ? "%.1f %s" : "%.0f %s", size, units[unit]);
}

int main(int argc, char **argv) {
    char cwd[PATH_MAX]; const char *root_path = argc > 1 ? argv[1] : (getcwd(cwd, sizeof(cwd)) ? cwd : ".");
    char resolved[PATH_MAX]; if (realpath(root_path, resolved)) root_path = resolved;
    Node *root = scan_path(root_path, root_path, 1);
    if (!root) { fprintf(stderr, "dupmap: cannot read '%s': %s\n", root_path, strerror(errno)); return EXIT_FAILURE; }

    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); start_color(); use_default_colors();
    for (int i = 1; i <= 6; ++i) init_pair(i, i, -1);
    Node *current = root; size_t selected = 0;
    for (;;) {
        int rows, cols; getmaxyx(stdscr, rows, cols); erase();
        char size_text[32]; format_size(current->size, size_text, sizeof(size_text));
        mvprintw(0, 0, "dupmap  %s  | %s", current->path, size_text);
        mvprintw(1, 0, "Arrows: select  Enter: open  Backspace: up  q: quit");
        BoxList boxes = {0}; layout_children(current, 0, 2, cols, rows - 4, &boxes);
        if (selected >= boxes.count && boxes.count) selected = boxes.count - 1;
        for (size_t i = 0; i < boxes.count; ++i) draw_box(&boxes.items[i], i == selected, 0);
        if (boxes.count) {
            char selected_size[32]; format_size(boxes.items[selected].node->size, selected_size, sizeof(selected_size));
            mvprintw(rows - 2, 0, "%s  (%s)%s", boxes.items[selected].node->path, selected_size,
                     boxes.items[selected].node->inaccessible ? " [permission denied]" : "");
        } else mvprintw(rows - 2, 0, "%s  (empty or inaccessible)", current->path);
        refresh();
        int key = getch();
        if (key == 'q' || key == 'Q') { free(boxes.items); break; }
        if (key == KEY_LEFT || key == KEY_UP) { if (selected) --selected; }
        else if (key == KEY_RIGHT || key == KEY_DOWN) { if (selected + 1 < boxes.count) ++selected; }
        else if ((key == '\n' || key == KEY_ENTER) && boxes.count && boxes.items[selected].node->is_dir && !boxes.items[selected].node->inaccessible) { current = boxes.items[selected].node; selected = 0; }
        else if ((key == KEY_BACKSPACE || key == 127 || key == 8) && current != root) {
            current = current->parent; selected = 0;
        }
        free(boxes.items);
    }
    endwin(); free_node(root); return EXIT_SUCCESS;
}
