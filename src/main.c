#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
#include <wctype.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#error "dupmap requires O_NOFOLLOW for safe filesystem access"
#endif

#define DUPMAP_VERSION "0.1.0"

typedef struct Node Node;
/*
 * One entry in the scanned filesystem tree. Directories own their child
 * nodes; parent is a non-owning link used when navigating upward. Directory
 * size and file_count are aggregates; size_known marks complete totals.
 * children_loaded records the cached directory snapshot.
 */
struct Node {
    char *name;
    char *path;
    Node *parent;
    off_t size;
    int is_dir;
    int inaccessible;
    int is_duplicate;
    int children_loaded;
    int size_known;
    time_t modified;
    dev_t device;
    ino_t inode;
    nlink_t link_count;
    size_t file_count;
    Node **children;
    size_t child_count;
    size_t child_cap;
};

typedef struct { Node **files; size_t count; off_t size; } DuplicateGroup;
static int sort_mode = 0; /* 0 size, 1 name, 2 modified */
static int ui_active;
static size_t ui_work_count;
static struct timespec ui_progress_started, ui_progress_last;

static size_t next_character(const char *text, mbstate_t *state, int *cells) {
    /*
     * mbrtowc advances by bytes while wcwidth measures terminal columns.
     * If the active locale cannot decode a byte sequence, consume one
     * plausible UTF-8 sequence as a single display cell so clipping still
     * makes progress and does not split a valid sequence in the common case.
     */
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

/* Return display-cell width rather than byte length for terminal rendering. */
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

/* Copy at most width terminal cells, reserving room for an ellipsis if clipped. */
static void clip_text(const char *text, int width, char *out, size_t out_size) {
    if (!out_size) return;
    if (width <= 0) { out[0] = '\0'; return; }
    int ellipsis = text_width(text) > width && width > 3 && out_size >= 4;
    int budget = width - (ellipsis ? 3 : 0);
    size_t copied = 0;
    mbstate_t state = {0};
    int used = 0, cells;
    while (*text) {
        size_t bytes = next_character(text, &state, &cells);
        size_t remaining = out_size - copied;
        if (!bytes || used + cells > budget || bytes >= remaining ||
            (ellipsis && remaining - bytes < 4)) break;
        memcpy(out + copied, text, bytes);
        copied += bytes;
        used += cells;
        text += bytes;
    }
    if (ellipsis && copied + 4 <= out_size) {
        memcpy(out + copied, "...", 4);
    } else out[copied] = '\0';
}

/* Preserve the end of a path, since its final components identify the location. */
static void clip_tail(const char *text, int width, char *out, size_t out_size) {
    if (!out_size) return;
    if (text_width(text) <= width) { clip_text(text, width, out, out_size); return; }
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
    if (out_size < 4) { clip_text(text + offset, width, out, out_size); return; }
    memcpy(out, "...", 3);
    size_t copied = 3;
    state = (mbstate_t){0};
    while (text[offset]) {
        size_t bytes = next_character(text + offset, &state, &cells);
        if (!bytes || bytes >= out_size - copied) break;
        memcpy(out + copied, text + offset, bytes);
        copied += bytes;
        offset += bytes;
    }
    out[copied] = '\0';
}

static void die(const char *message) {
    if (ui_active) endwin();
    fprintf(stderr, "dupmap: %s\n", message);
    exit(EXIT_FAILURE);
}

/* Grow vectors geometrically while checking every allocation multiplication. */
static void *grow_array(void *items, size_t *capacity, size_t item_size,
                        size_t initial_capacity) {
    size_t next = *capacity ? *capacity : initial_capacity;
    if (*capacity) {
        if (*capacity > SIZE_MAX / 2) die("too many entries");
        next = *capacity * 2;
    }
    if (!item_size || next > SIZE_MAX / item_size) die("too many entries");
    void *grown = realloc(items, next * item_size);
    if (!grown) die("out of memory");
    *capacity = next;
    return grown;
}

/* Replace terminal controls and malformed UTF-8 before rendering user names. */
static void safe_terminal_text(const char *text, char *out, size_t out_size) {
    if (!out_size) return;
    size_t copied = 0;
    mbstate_t state = {0};
    while (*text && copied + 1 < out_size) {
        wchar_t character;
        size_t bytes = mbrtowc(&character, text, MB_CUR_MAX, &state);
        /* Filenames are untrusted terminal input: controls could move the cursor. */
        if (bytes == (size_t)-1 || bytes == (size_t)-2) {
            out[copied++] = '?';
            ++text;
            memset(&state, 0, sizeof(state));
            continue;
        }
        if (!bytes) break;
        if (iswcntrl(character)) out[copied++] = '?';
        else {
            if (copied + bytes >= out_size) break;
            memcpy(out + copied, text, bytes);
            copied += bytes;
        }
        text += bytes;
    }
    out[copied] = '\0';
}

/* O_NOFOLLOW blocks final-component symlinks; fstat rejects special files. */
static int open_regular_file(const char *path) {
    /* NONBLOCK prevents a raced-in FIFO from hanging before fstat can reject it. */
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

static off_t max_off_t_value(void) {
    /* Saturate aggregate totals instead of allowing signed off_t overflow. */
    unsigned shift = (unsigned)(sizeof(uintmax_t) * CHAR_BIT - sizeof(off_t) * CHAR_BIT + 1);
    return (off_t)(UINTMAX_MAX >> shift);
}

/* Throttle loading redraws; poll input so long operations can be quit. */
static void show_progress(const char *phase, const char *path) {
    if (!ui_active) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double since_draw = (double)(now.tv_sec - ui_progress_last.tv_sec) +
                        (now.tv_nsec - ui_progress_last.tv_nsec) / 1e9;
    if (since_draw < 0.1) return;
    ui_progress_last = now;
    double elapsed = (double)(now.tv_sec - ui_progress_started.tv_sec) +
                     (now.tv_nsec - ui_progress_started.tv_nsec) / 1e9;
    nodelay(stdscr, TRUE);
    int key = getch();
    nodelay(stdscr, FALSE);
    if (key == 'q' || key == 'Q') { endwin(); exit(EXIT_SUCCESS); }
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (rows < 1 || cols < 1) return;
    char message[PATH_MAX + 80];
    char visible_path[PATH_MAX + 32];
    safe_terminal_text(path, visible_path, sizeof(visible_path));
    erase();
    snprintf(message, sizeof(message), "%c %s", "|/-\\"[(unsigned)(elapsed * 10) % 4], phase);
    char clipped[PATH_MAX + 80];
    clip_text(message, cols, clipped, sizeof(clipped));
    mvaddnstr(0, 0, clipped, (int)strlen(clipped));
    if (rows > 2) {
        snprintf(message, sizeof(message), "Entries processed: %zu | Elapsed: %.1fs", ui_work_count, elapsed);
        clip_text(message, cols, clipped, sizeof(clipped));
        mvaddnstr(2, 0, clipped, (int)strlen(clipped));
    }
    if (rows > 4) {
        clip_tail(visible_path, cols, clipped, sizeof(clipped));
        mvaddnstr(4, 0, clipped, (int)strlen(clipped));
    }
    if (rows > 6) {
        clip_text("Working... Q to quit", cols, clipped, sizeof(clipped));
        mvaddnstr(6, 0, clipped, (int)strlen(clipped));
    }
    refresh();
}

static void begin_progress(const char *phase, const char *path) {
    ui_work_count = 0;
    clock_gettime(CLOCK_MONOTONIC, &ui_progress_started);
    ui_progress_last = (struct timespec){0};
    show_progress(phase, path);
}

/* Return a heap-owned copy; allocation failures use the common fatal path. */
static char *copy_string(const char *s) {
    char *copy = strdup(s);
    if (!copy) die("out of memory");
    return copy;
}

/* Join a parent path and one directory entry, checking size arithmetic first. */
static char *join_path(const char *parent, const char *name) {
    size_t n = strlen(parent), m = strlen(name), slash = (n && parent[n - 1] != '/') ? 1 : 0;
    if (m > SIZE_MAX - n || n + m > SIZE_MAX - slash - 1) die("path is too long");
    char *out = malloc(n + slash + m + 1);
    if (!out) die("out of memory");
    memcpy(out, parent, n);
    if (slash) out[n] = '/';
    memcpy(out + n + slash, name, m + 1);
    return out;
}

/* Allocate a zero-initialized tree node and take ownership of copied strings. */
static Node *new_node(const char *name, const char *path, int is_dir) {
    Node *node = calloc(1, sizeof(*node));
    if (!node) die("out of memory");
    node->name = copy_string(name);
    node->path = copy_string(path);
    node->is_dir = is_dir;
    node->file_count = is_dir ? 0 : 1;
    return node;
}

/* Attach a child, set its back-link, and accumulate saturated directory totals. */
static void add_child(Node *parent, Node *child) {
    if (parent->child_count == parent->child_cap) {
        parent->children = grow_array(parent->children, &parent->child_cap,
                                      sizeof(*parent->children), 32);
    }
    parent->children[parent->child_count++] = child;
    child->parent = parent;
    off_t max_size = max_off_t_value();
    if (child->size > 0 && parent->size > max_size - child->size) parent->size = max_size;
    else parent->size += child->size;
    if (child->file_count > SIZE_MAX - parent->file_count) parent->file_count = SIZE_MAX;
    else parent->file_count += child->file_count;
}

/* qsort comparator: selected mode first, then case-insensitive name for ties. */
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

/* Keep each directory's children in the same order used by the UI. */
static void sort_children(Node *parent) {
    if (parent->child_count)
        qsort(parent->children, parent->child_count, sizeof(*parent->children), compare_nodes);
}

/* Hashes only shortlist candidates; byte comparison remains authoritative. */
typedef struct { Node *node; uint64_t hash; } HashedFile;
typedef struct { HashedFile *items; size_t count, cap; } FileList;

static int compare_hashed_files(const void *a, const void *b) {
    const HashedFile *left = a, *right = b;
    if (left->node->size < right->node->size) return -1;
    if (left->node->size > right->node->size) return 1;
    if (left->hash < right->hash) return -1;
    if (left->hash > right->hash) return 1;
    return strcmp(left->node->path, right->node->path);
}

static int compare_file_sizes(const void *a, const void *b) {
    const HashedFile *left = a, *right = b;
    if (left->node->size < right->node->size) return -1;
    if (left->node->size > right->node->size) return 1;
    return strcmp(left->node->path, right->node->path);
}

/* Compute a streaming FNV-1a hash; open failures are represented by zero. */
static uint64_t file_hash(const char *path) {
    int fd = open_regular_file(path);
    if (fd < 0) return 0;
    FILE *file = fdopen(fd, "rb");
    if (!file) { close(fd); return 0; }
    uint64_t hash = UINT64_C(14695981039346656037);
    unsigned char buffer[8192]; size_t got;
    size_t chunks = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        for (size_t i = 0; i < got; ++i) { hash ^= buffer[i]; hash *= UINT64_C(1099511628211); }
        if ((++chunks & 127) == 0) show_progress("Hashing files", path);
    }
    int failed = ferror(file); fclose(file);
    return failed ? 0 : hash;
}

/* Hashes only shortlist candidates; compare bytes to confirm exact equality. */
static int files_equal(const char *left_path, const char *right_path) {
    int left_fd = open_regular_file(left_path), right_fd = open_regular_file(right_path);
    if (left_fd < 0 || right_fd < 0) {
        if (left_fd >= 0) close(left_fd);
        if (right_fd >= 0) close(right_fd);
        return 0;
    }
    FILE *left = fdopen(left_fd, "rb"), *right = fdopen(right_fd, "rb");
    if (!left || !right) {
        if (left) fclose(left); else close(left_fd);
        if (right) fclose(right); else close(right_fd);
        return 0;
    }
    unsigned char a[8192], b[8192]; size_t na, nb; int equal = 1;
    size_t chunks = 0;
    do {
        na = fread(a, 1, sizeof(a), left); nb = fread(b, 1, sizeof(b), right);
        if ((++chunks & 127) == 0) show_progress("Comparing files", left_path);
        if (na != nb || memcmp(a, b, na) != 0) { equal = 0; break; }
    } while (na > 0);
    if (ferror(left) || ferror(right)) equal = 0;
    fclose(left); fclose(right); return equal;
}

/* Flatten the tree into file references; the root tree retains node ownership. */
static void collect_files(Node *node, FileList *list) {
    ++ui_work_count;
    if ((ui_work_count & 255) == 0) show_progress("Collecting files", node->path);
    if (!node->is_dir) {
        if (list->count == list->cap) {
            list->items = grow_array(list->items, &list->cap, sizeof(*list->items), 64);
        }
        list->items[list->count++].node = node; return;
    }
    for (size_t i = 0; i < node->child_count; ++i) collect_files(node->children[i], list);
}

/*
 * Narrow duplicate candidates by size and then hash. Byte comparison is the
 * final authority, so hash collisions never imply equality. The returned
 * groups own their pointer arrays, but not the referenced tree nodes.
 */
static size_t find_duplicate_groups(Node *root, DuplicateGroup **out) {
    FileList files = {0}; collect_files(root, &files);
    unsigned char *used = calloc(files.count, 1);
    DuplicateGroup *groups = NULL; size_t count = 0, cap = 0;
    if (!used && files.count) die("out of memory");
    if (files.count > 1)
        qsort(files.items, files.count, sizeof(*files.items), compare_file_sizes);
    /* Size sort avoids opening unique-size files. Hash sort narrows exact
       comparisons to same-size, same-hash buckets: typical work is O(n log n)
       plus reads for repeated sizes and byte verification of likely matches. */
    for (size_t size_bucket = 0; size_bucket < files.count;) {
        size_t size_end = size_bucket + 1;
        while (size_end < files.count &&
               files.items[size_end].node->size == files.items[size_bucket].node->size) ++size_end;
        if (size_end - size_bucket > 1) {
            for (size_t i = size_bucket; i < size_end; ++i) {
                files.items[i].hash = file_hash(files.items[i].node->path);
                ui_work_count = i + 1;
                if ((i & 63) == 0) show_progress("Checking duplicates", files.items[i].node->name);
            }
            qsort(files.items + size_bucket, size_end - size_bucket,
                  sizeof(*files.items), compare_hashed_files);
            for (size_t hash_bucket = size_bucket; hash_bucket < size_end;) {
                size_t hash_end = hash_bucket + 1;
                while (hash_end < size_end &&
                       files.items[hash_end].hash == files.items[hash_bucket].hash) ++hash_end;
                for (size_t i = hash_bucket; i < hash_end; ++i) {
                    if (used[i]) continue;
                    Node **matches = NULL; size_t match_count = 0, match_cap = 0;
                    for (size_t j = i; j < hash_end; ++j) {
                        if (used[j]) continue;
                        int equal = match_count == 0 || files_equal(files.items[i].node->path, files.items[j].node->path);
                        if (!equal) continue;
                        if (match_count == match_cap)
                            matches = grow_array(matches, &match_cap, sizeof(*matches), 4);
                        matches[match_count++] = files.items[j].node;
                        used[j] = 1;
                    }
                    if (match_count > 1) {
                        for (size_t j = 0; j < match_count; ++j) matches[j]->is_duplicate = 1;
                        if (count == cap) groups = grow_array(groups, &cap, sizeof(*groups), 8);
                        groups[count++] = (DuplicateGroup){matches, match_count, files.items[i].node->size};
                    } else free(matches);
                }
                hash_bucket = hash_end;
            }
        }
        size_bucket = size_end;
    }
    free(used); free(files.items); *out = groups; return count;
}

/* Release group arrays only; each referenced Node belongs to the scanned tree. */
static void free_duplicate_groups(DuplicateGroup *groups, size_t count) {
    for (size_t i = 0; i < count; ++i) free(groups[i].files);
    free(groups);
}

static int compare_file_identity(const void *a, const void *b) {
    const Node *left = *(const Node * const *)a, *right = *(const Node * const *)b;
    if (left->device < right->device) return -1;
    if (left->device > right->device) return 1;
    if (left->inode < right->inode) return -1;
    if (left->inode > right->inode) return 1;
    return 0;
}

/* An inode can release its data only when every hard link is removed.
   Keep one copy, preferring an inode whose links extend outside this group. */
static off_t duplicate_reclaimable_size(const DuplicateGroup *group) {
    if (group->count < 2 || group->size <= 0) return 0;
    if (group->count > SIZE_MAX / sizeof(Node *)) die("too many duplicate files");
    Node **files = malloc(group->count * sizeof(*files));
    if (!files) die("out of memory");
    memcpy(files, group->files, group->count * sizeof(*files));
    qsort(files, group->count, sizeof(*files), compare_file_identity);
    size_t inodes = 0, reclaimable = 0;
    for (size_t first = 0; first < group->count;) {
        size_t end = first + 1;
        while (end < group->count && files[end]->device == files[first]->device &&
               files[end]->inode == files[first]->inode) ++end;
        ++inodes;
        if (files[first]->link_count && end - first == files[first]->link_count)
            ++reclaimable;
        first = end;
    }
    free(files);
    if (reclaimable == inodes) --reclaimable;
    off_t max_size = max_off_t_value();
    return reclaimable > (uintmax_t)(max_size / group->size)
        ? max_size : group->size * (off_t)reclaimable;
}

/* Directory totals remain incomplete until every descendant is loaded. */
static void update_totals(Node *dir) {
    dir->size = 0;
    dir->file_count = 0;
    dir->size_known = dir->children_loaded && !dir->inaccessible;
    off_t maximum = max_off_t_value();
    for (size_t i = 0; i < dir->child_count; ++i) {
        Node *child = dir->children[i];
        if (!child->size_known) dir->size_known = 0;
        dir->size = dir->size > maximum - child->size ? maximum : dir->size + child->size;
        dir->file_count = child->file_count > SIZE_MAX - dir->file_count
            ? SIZE_MAX : dir->file_count + child->file_count;
    }
}

/* Only classify an entry; directory contents are loaded separately. */
static Node *scan_entry(const char *path, const char *display_name) {
    ++ui_work_count;
    show_progress("Scanning folders", path);
    struct stat st;
    if (lstat(path, &st) != 0) return NULL;
    if (S_ISLNK(st.st_mode)) return NULL;
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) { errno = ENOTSUP; return NULL; }
    Node *node = new_node(display_name, path, S_ISDIR(st.st_mode));
    node->modified = st.st_mtime;
    node->device = st.st_dev;
    node->inode = st.st_ino;
    node->link_count = st.st_nlink;
    if (!node->is_dir) { node->size = st.st_size; node->size_known = 1; }
    return node;
}

/* Load once, retaining node addresses used by selections and duplicate groups.
   Verify the original inode and never follow a replacement symlink. */
static void load_children(Node *dir) {
    if (!dir->is_dir || dir->children_loaded) return;
    dir->children_loaded = 1;
    int directory_fd = open(dir->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat opened_st;
    if (directory_fd < 0 || fstat(directory_fd, &opened_st) != 0 ||
        !S_ISDIR(opened_st.st_mode) || opened_st.st_dev != dir->device || opened_st.st_ino != dir->inode) {
        if (directory_fd >= 0) close(directory_fd);
        dir->inaccessible = 1;
    } else {
        DIR *handle = fdopendir(directory_fd);
        if (!handle) { close(directory_fd); dir->inaccessible = 1; }
        else {
            for (;;) {
                errno = 0;
                struct dirent *entry = readdir(handle);
                if (!entry) { if (errno) dir->inaccessible = 1; break; }
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
                char *child_path = join_path(dir->path, entry->d_name);
                Node *child = scan_entry(child_path, entry->d_name);
                free(child_path);
                if (child) add_child(dir, child);
            }
            if (closedir(handle) != 0) dir->inaccessible = 1;
        }
    }
    update_totals(dir);
    sort_children(dir);
}

/* Complete metadata traversal only for an explicitly requested full scan. */
static void load_tree(Node *node) {
    if (!node->is_dir) return;
    load_children(node);
    for (size_t i = 0; i < node->child_count; ++i) load_tree(node->children[i]);
    update_totals(node);
}

static Node *scan_path(const char *path, const char *display_name, int recursive) {
    Node *node = scan_entry(path, display_name);
    if (node && node->is_dir) {
        if (recursive) { load_tree(node); sort_children(node); }
        else load_children(node);
    }
    return node;
}

/* Recursively release the tree; each child is owned exactly once by its parent. */
static void free_node(Node *node) {
    if (!node) return;
    for (size_t i = 0; i < node->child_count; ++i) free_node(node->children[i]);
    free(node->children); free(node->name); free(node->path); free(node);
}

/* Format byte counts using binary units while keeping output bounded. */
static void format_size(off_t value, char *out, size_t length) {
    const char *units[] = {"B", "K", "M", "G", "T", "P", "E"}; double size = (double)value; int unit = 0;
    while (size >= 1024 && unit < 6) { size /= 1024; ++unit; }
    snprintf(out, length, unit ? "%.1f %s" : "%.0f %s", size, units[unit]);
}

static void format_node_size(const Node *node, char *out, size_t length) {
    if (node->is_dir && !node->size_known) snprintf(out, length, "not calculated");
    else format_size(node->size, out, length);
}

static const char *sort_name(void) {
    return sort_mode == 1 ? "name" : (sort_mode == 2 ? "modified" : "size");
}

/* Resolve the per-user preference directory and state-file path. */
static int state_paths(char *directory, size_t directory_size,
                       char *path, size_t path_size) {
    const char *base = getenv("XDG_STATE_HOME");
    int length;
    if (base && *base) {
        if (base[0] != '/') return 0;
        length = snprintf(directory, directory_size, "%s/dupmap", base);
    }
    else {
        const char *home = getenv("HOME");
        if (!home || !*home || home[0] != '/') return 0;
        length = snprintf(directory, directory_size, "%s/.config/dupmap", home);
    }
    if (length < 0 || (size_t)length >= directory_size) return 0;
    size_t dir_length = (size_t)length;
    if (dir_length > path_size || sizeof("/state") > path_size - dir_length) return 0;
    memcpy(path, directory, dir_length);
    memcpy(path + dir_length, "/state", sizeof("/state"));
    return 1;
}

/* State lives in a private, user-owned directory and never controls file ops. */
static int state_directory_is_safe(const char *directory) {
    /* Reject symlinked, foreign-owned, or group/world-writable state folders. */
    struct stat st;
    return lstat(directory, &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == geteuid() && !(st.st_mode & (S_IWGRP | S_IWOTH));
}

/* Open preferences without following links; require a user-owned regular file. */
static FILE *open_state_file(const char *path, int write_access) {
    int flags = (write_access ? O_WRONLY | O_CREAT : O_RDONLY) |
                O_CLOEXEC | O_NOFOLLOW;
    int fd = open(path, flags, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        fchmod(fd, 0600) != 0 || (write_access && ftruncate(fd, 0) != 0)) {
        close(fd);
        errno = EPERM;
        return NULL;
    }
    FILE *file = fdopen(fd, write_access ? "w" : "r");
    if (!file) close(fd);
    return file;
}

/* Treat malformed preference data as optional state, not as trusted input. */
/* Parse exactly three bounded integer preferences and reject trailing data. */
static int read_state_modes(FILE *file, int *saved_sort, int *saved_color,
                            int *saved_list) {
    char line[128];
    if (!fgets(line, sizeof(line), file) || (!strchr(line, '\n') && !feof(file))) return 0;
    char *cursor = line;
    long values[3];
    for (size_t i = 0; i < 3; ++i) {
        while (isspace((unsigned char)*cursor)) ++cursor;
        errno = 0;
        char *end;
        values[i] = strtol(cursor, &end, 10);
        if (errno || end == cursor || values[i] < 0 || values[i] > (i == 2 ? 1 : 2)) return 0;
        cursor = end;
    }
    while (isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor) return 0;
    *saved_sort = (int)values[0];
    *saved_color = (int)values[1];
    *saved_list = (int)values[2];
    return 1;
}

/* Load optional preferences and accept the remembered location only if valid. */
static int load_state(char *last_path, size_t path_length, int *saved_sort, int *saved_color, int *saved_list) {
    char state_dir[PATH_MAX], state_path[PATH_MAX];
    if (!state_paths(state_dir, sizeof(state_dir), state_path, sizeof(state_path)) ||
        !state_directory_is_safe(state_dir)) return 0;
    FILE *file = open_state_file(state_path, 0);
    if (!file) return 0;
    if (!fgets(last_path, (int)path_length, file) ||
        (!strchr(last_path, '\n') && !feof(file))) { fclose(file); return 0; }
    last_path[strcspn(last_path, "\r\n")] = '\0';
    /* A valid remembered path still works if only the optional mode line is bad. */
    if (!read_state_modes(file, saved_sort, saved_color, saved_list))
        *saved_sort = *saved_color = *saved_list = 0;
    fclose(file);
    struct stat st;
    return *last_path && stat(last_path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Persist location and view preferences; inability to save is nonfatal. */
static void save_state(const char *last_path, int saved_sort, int saved_color, int saved_list) {
    char state_dir[PATH_MAX], state_path[PATH_MAX];
    /* The compact state format is line-oriented; don't persist ambiguous paths. */
    if (strchr(last_path, '\n') || strchr(last_path, '\r')) return;
    if (!state_paths(state_dir, sizeof(state_dir), state_path, sizeof(state_path))) return;
    if (mkdir(state_dir, 0700) != 0 && errno != EEXIST) return;
    if (!state_directory_is_safe(state_dir)) return;
    FILE *file = open_state_file(state_path, 1);
    if (!file) return;
    fprintf(file, "%s\n%d %d %d\n", last_path, saved_sort, saved_color, saved_list);
    fclose(file);
}

/* Case-insensitive substring search used by the interactive list filter. */
static int name_matches(const char *name, const char *query) {
    if (!query[0]) return 1;
    for (const char *start = name; *start; ++start) {
        const char *a = start, *b = query;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
        if (!*b) return 1;
    }
    return 0;
}

enum View { VIEW_DASHBOARD, VIEW_FOLDERS, VIEW_FILES, VIEW_DUPLICATES, VIEW_ALL, VIEW_COUNT };
enum { CARD_WIDTH = 26, CARD_HEIGHT = 4 };
typedef struct { size_t selected, first; char filter[256]; } ViewState;
typedef struct { Node *node; size_t group; int heading; } ViewRow;
typedef struct { ViewRow *items; size_t count, cap; } ViewRows;

static const char *view_name(enum View view) {
    const char *names[] = {"Dashboard", "Folders", "Files", "Duplicates", "All items"};
    return names[view];
}

static int view_accepts_node(enum View view, const Node *node) {
    return view == VIEW_ALL || (view == VIEW_FILES ? !node->is_dir :
           ((view == VIEW_DASHBOARD || view == VIEW_FOLDERS) && node->is_dir));
}

static void add_view_row(ViewRows *rows, Node *node, size_t group, int heading) {
    if (rows->count == rows->cap)
        rows->items = grow_array(rows->items, &rows->cap, sizeof(*rows->items), 32);
    rows->items[rows->count++] = (ViewRow){node, group, heading};
}

/* Duplicate membership is scan-wide. Expanded paths are individually scrollable. */
static void build_view_rows(Node *current, enum View view, const char *filter,
                            DuplicateGroup *groups, size_t group_count,
                            size_t expanded, ViewRows *rows) {
    rows->count = 0;
    if (view != VIEW_DUPLICATES) {
        for (size_t i = 0; i < current->child_count; ++i) {
            Node *node = current->children[i];
            if (view_accepts_node(view, node) && name_matches(node->name, filter))
                add_view_row(rows, node, SIZE_MAX, 0);
        }
        return;
    }
    for (size_t i = 0; i < group_count; ++i) {
        int matches = !filter[0];
        for (size_t j = 0; j < groups[i].count && !matches; ++j)
            matches = name_matches(groups[i].files[j]->path, filter);
        if (!matches) continue;
        add_view_row(rows, groups[i].files[0], i, 1);
        if (i == expanded)
            for (size_t j = 0; j < groups[i].count; ++j)
                add_view_row(rows, groups[i].files[j], i, 0);
    }
}

static int card_geometry(int width, int height, int *columns, int *rows) {
    *columns = *rows = 0;
    if (width < CARD_WIDTH || height < CARD_HEIGHT) return 0;
    *columns = 1 + (width - CARD_WIDTH) / (CARD_WIDTH + 2);
    *rows = 1 + (height - CARD_HEIGHT) / (CARD_HEIGHT + 1);
    return 1;
}

/* Keep the selected item visible even when terminal dimensions change. */
static void keep_visible(ViewState *state, size_t count, size_t page, size_t stride) {
    if (!count) { state->selected = state->first = 0; return; }
    if (state->selected >= count) state->selected = count - 1;
    if (state->first >= count) state->first = 0;
    state->first -= state->first % stride;
    if (state->selected < state->first)
        state->first = state->selected / stride * stride;
    else if (state->selected - state->first >= page)
        state->first = (state->selected - page + stride) / stride * stride;
}

static void select_anchor(const ViewRows *rows, ViewState *state, ViewRow anchor) {
    for (size_t i = 0; i < rows->count; ++i)
        if (rows->items[i].node == anchor.node && rows->items[i].group == anchor.group &&
            rows->items[i].heading == anchor.heading) { state->selected = i; return; }
}

/* All directory tabs share the sorted children; preserve each tab's node. */
static void sort_view_states(Node *current, ViewState states[VIEW_COUNT]) {
    ViewRow anchors[VIEW_COUNT] = {0};
    ViewRows rows = {0};
    for (enum View view = VIEW_DASHBOARD; view < VIEW_COUNT; ++view) {
        if (view == VIEW_DUPLICATES) continue;
        build_view_rows(current, view, states[view].filter, NULL, 0, SIZE_MAX, &rows);
        if (states[view].selected < rows.count) anchors[view] = rows.items[states[view].selected];
    }
    sort_children(current);
    for (enum View view = VIEW_DASHBOARD; view < VIEW_COUNT; ++view) {
        if (!anchors[view].node) continue;
        build_view_rows(current, view, states[view].filter, NULL, 0, SIZE_MAX, &rows);
        select_anchor(&rows, &states[view], anchors[view]);
    }
    free(rows.items);
}

/* Sanitize user names, then clip by terminal cells before any curses write. */
static void draw_text(int y, int x, int width, const char *text) {
    if (width <= 0) return;
    char safe[PATH_MAX * 4 + 256], clipped[PATH_MAX * 4 + 256];
    safe_terminal_text(text, safe, sizeof(safe));
    clip_text(safe, width, clipped, sizeof(clipped));
    mvaddnstr(y, x, clipped, (int)strlen(clipped));
}

static void draw_card(Node *node, int x, int y, int selected) {
    int attrs = selected ? A_REVERSE | A_BOLD : A_NORMAL;
    attron(attrs);
    mvaddch(y, x, '+'); mvhline(y, x + 1, '-', CARD_WIDTH - 2);
    mvaddch(y, x + CARD_WIDTH - 1, '+');
    for (int row = 1; row < CARD_HEIGHT - 1; ++row) {
        mvaddch(y + row, x, '|'); mvhline(y + row, x + 1, ' ', CARD_WIDTH - 2);
        mvaddch(y + row, x + CARD_WIDTH - 1, '|');
    }
    mvaddch(y + CARD_HEIGHT - 1, x, '+');
    mvhline(y + CARD_HEIGHT - 1, x + 1, '-', CARD_WIDTH - 2);
    mvaddch(y + CARD_HEIGHT - 1, x + CARD_WIDTH - 1, '+');
    draw_text(y + 1, x + 2, CARD_WIDTH - 4, node->name);
    char size[32], detail[128]; format_size(node->size, size, sizeof(size));
    if (node->inaccessible) snprintf(detail, sizeof(detail), "Cannot read folder");
    else if (!node->size_known) snprintf(detail, sizeof(detail), "Size not calculated");
    else snprintf(detail, sizeof(detail), "%s | %zu %s", size, node->file_count,
                  node->file_count == 1 ? "file" : "files");
    draw_text(y + 2, x + 2, CARD_WIDTH - 4, detail);
    attroff(attrs);
}

static void filter_backspace(char *filter) {
    size_t length = strlen(filter);
    if (!length) return;
    --length;
    while (length && ((unsigned char)filter[length] & 0xC0) == 0x80) --length;
    filter[length] = '\0';
}

static const char *keyboard_hint(int cols) {
    if (cols >= 76) return "Arrows move | Enter open | Backspace up | F filter | S sort | ? help | Q quit";
    if (cols >= 48) return "Enter open | Backspace up | F filter | ? help | Q quit";
    return "Enter open | ? help | Q quit";
}

static int run_dashboard(Node *root) {
    DuplicateGroup *groups = NULL;
    size_t group_count = 0;
    off_t *reclaimable = NULL;
    int duplicates_checked = 0;
    off_t total_reclaimable = 0, max_size = max_off_t_value();
    Node *current = root;
    enum View view = VIEW_DASHBOARD, previous_view = VIEW_DASHBOARD;
    ViewState states[VIEW_COUNT] = {0};
    ViewRows items = {0};
    size_t expanded = SIZE_MAX;
    int help = 0, editing = 0, have_anchor = 0;
    ViewState before_edit = {0};
    ViewRow anchor = {0};
    for (;;) {
        /* Reading file contents is deferred until D, then cached even if empty. */
        if (view == VIEW_DUPLICATES && !duplicates_checked) {
            begin_progress("Checking duplicates", root->path);
            group_count = find_duplicate_groups(root, &groups);
            if (group_count > SIZE_MAX / sizeof(*reclaimable)) die("too many duplicate groups");
            reclaimable = group_count ? malloc(group_count * sizeof(*reclaimable)) : NULL;
            if (group_count && !reclaimable) die("out of memory");
            for (size_t i = 0; i < group_count; ++i) {
                reclaimable[i] = duplicate_reclaimable_size(&groups[i]);
                total_reclaimable = total_reclaimable > max_size - reclaimable[i]
                    ? max_size : total_reclaimable + reclaimable[i];
            }
            duplicates_checked = 1;
        }
        ViewState *state = &states[view];
        build_view_rows(current, view, state->filter, groups, group_count, expanded, &items);
        if (have_anchor) { select_anchor(&items, state, anchor); have_anchor = 0; }
        int rows, cols; getmaxyx(stdscr, rows, cols); erase();
        int top = 3, body_height = rows - top - 3;
        int columns = 1, card_rows = 0, cards = 0;
        size_t page = 1;
        if (rows < 7 || cols < 16) {
            draw_text(0, 0, cols, "Terminal too small; resize or Q quit");
            keep_visible(state, items.count, 1, 1);
        } else {
            char safe_path[PATH_MAX * 4], path[PATH_MAX * 4];
            safe_terminal_text(view == VIEW_DUPLICATES ? root->path : current->path, safe_path, sizeof(safe_path));
            clip_tail(safe_path, cols - 9, path, sizeof(path));
            attron(A_BOLD); draw_text(0, 0, 7, "dupmap"); attroff(A_BOLD);
            draw_text(0, 8, cols - 8, path);
            draw_text(1, 0, cols, cols >= 62
                ? "1 Dashboard | 2 Folders | 3 Files | D Duplicates | L All items"
                : "1 Home  2 Dirs  3 Files  D Dupes  L All");
            char title[320];
            snprintf(title, sizeof(title), "%s%s%s", view_name(view),
                     state->filter[0] ? " | Filter: " : "", state->filter);
            attron(A_BOLD); draw_text(2, 0, cols, title); attroff(A_BOLD);
            if (view == VIEW_DASHBOARD && rows >= 16 && cols >= 48 && !help) {
                size_t folders = 0, files = 0;
                for (size_t i = 0; i < current->child_count; ++i)
                    if (current->children[i]->is_dir) ++folders; else ++files;
                char size[32], reclaim[32], summary[192];
                format_node_size(current, size, sizeof(size));
                format_size(total_reclaimable, reclaim, sizeof(reclaim));
                snprintf(summary, sizeof(summary), "Total: %s | Folders: %zu | Files here: %zu", size, folders, files);
                draw_text(3, 0, cols, summary);
                if (duplicates_checked)
                    snprintf(summary, sizeof(summary), "Duplicate groups: %zu (scan-wide) | Reclaimable: %s", group_count, reclaim);
                else snprintf(summary, sizeof(summary), "Duplicates: not checked | Press D to scan");
                draw_text(4, 0, cols, summary);
                draw_text(5, 0, cols, "FOLDERS - Enter opens the selected folder");
                top = 6;
            }
            body_height = rows - top - 3;
            cards = (view == VIEW_DASHBOARD || view == VIEW_FOLDERS) &&
                    card_geometry(cols, body_height, &columns, &card_rows);
            if (!cards) columns = 1;
            page = cards ? (size_t)columns * card_rows : (size_t)body_height;
            keep_visible(state, items.count, page, cards ? (size_t)columns : 1);
            if (help) {
                const char *lines[] = {
                    "1 Dashboard   2 Folders   3 Files", "D Duplicates (scan-wide)   L All items",
                    "Arrows move   PgUp/PgDn page", "Home/End first/last   Enter open/expand",
                    "Backspace parent   Esc return from duplicates", "F filter   Ctrl-U clear filter   S sort",
                    "[D!] marks an unreadable folder",
                    "? or Esc close help   Q quit"
                };
                for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]) && (int)i < body_height; ++i)
                    draw_text(top + (int)i, 0, cols, lines[i]);
            } else if (!items.count) {
                draw_text(top, 0, cols, state->filter[0] ? "No items match the current filter"
                    : current->inaccessible && view != VIEW_DUPLICATES ? "Cannot read this directory"
                    : view == VIEW_DUPLICATES ? "No duplicate files in the scanned tree"
                    : view == VIEW_FILES ? "No files directly in this folder"
                    : view == VIEW_ALL ? "This directory is empty"
                    : "No folders here - press 3 to see files");
            } else {
                size_t shown = items.count - state->first;
                if (shown > page) shown = page;
                for (size_t offset = 0; offset < shown; ++offset) {
                    size_t index = state->first + offset;
                    ViewRow item = items.items[index];
                    int selected = index == state->selected;
                    if (cards) {
                        draw_card(item.node, (int)(offset % (size_t)columns) * (CARD_WIDTH + 2),
                                  top + (int)(offset / (size_t)columns) * (CARD_HEIGHT + 1), selected);
                    } else {
                        char size[32], line[PATH_MAX * 4 + 256];
                        format_node_size(item.node, size, sizeof(size));
                        if (view == VIEW_DUPLICATES && item.heading) {
                            char reclaim[32]; format_size(reclaimable[item.group], reclaim, sizeof(reclaim));
                            snprintf(line, sizeof(line), "%c [%c] Group %zu | %zu files | %s each | %s reclaimable",
                                     selected ? '>' : ' ', item.group == expanded ? '-' : '+', item.group + 1,
                                     groups[item.group].count, size, reclaim);
                        } else if (view == VIEW_DUPLICATES)
                            snprintf(line, sizeof(line), "%c   %s", selected ? '>' : ' ', item.node->path);
                        else {
                            char modified[32] = "";
                            struct tm *date = localtime(&item.node->modified);
                            if (date) strftime(modified, sizeof(modified), "%Y-%m-%d", date);
                            char safe_name[PATH_MAX * 4], short_name[PATH_MAX * 4];
                            safe_terminal_text(item.node->name, safe_name, sizeof(safe_name));
                            const char *kind = item.node->inaccessible ? "D!" : item.node->is_dir ? "D" : "F";
                            int name_width = cols - 5 - (int)strlen(kind);
                            if (cols >= 40) name_width -= (int)strlen(size) + 3;
                            if (cols >= 76) name_width -= (int)strlen(modified) + 3;
                            clip_text(safe_name, name_width, short_name, sizeof(short_name));
                            snprintf(line, sizeof(line), "%c [%s] %s%s%s%s%s", selected ? '>' : ' ',
                                     kind, short_name,
                                     cols >= 40 ? " | " : "", cols >= 40 ? size : "",
                                     cols >= 76 ? " | " : "", cols >= 76 ? modified : "");
                        }
                        if (selected) attron(A_REVERSE | A_BOLD);
                        draw_text(top + (int)offset, 0, cols, line);
                        if (selected) attroff(A_REVERSE | A_BOLD);
                    }
                }
            }
            char range[160];
            size_t end = items.count - state->first;
            if (end > page) end = page;
            snprintf(range, sizeof(range), "%s %zu-%zu of %zu%s | Sort: %s", view_name(view),
                     items.count ? state->first + 1 : 0, state->first + end, items.count,
                     state->first + end < items.count ? " | More below" : "", sort_name());
            draw_text(rows - 3, 0, cols, range);
            if (editing) {
                char prompt[320]; snprintf(prompt, sizeof(prompt), "Filter: %s | Enter apply, Esc cancel", state->filter);
                draw_text(rows - 2, 0, cols, prompt);
                int cursor = 8 + text_width(state->filter);
                if (cursor >= cols) cursor = cols - 1;
                move(rows - 2, cursor);
            } else if (help) draw_text(rows - 2, 0, cols, "Help is open | ? or Esc closes");
            else if (items.count) {
                ViewRow item = items.items[state->selected];
                if (view == VIEW_DUPLICATES && item.heading)
                    draw_text(rows - 2, 0, cols, "Enter expands/collapses this group | Esc returns");
                else {
                    char safe[PATH_MAX * 4], tail[PATH_MAX * 4];
                    safe_terminal_text(item.node->path, safe, sizeof(safe));
                    int prefix = item.node->inaccessible ? 13 : 0;
                    clip_tail(safe, cols - prefix, tail, sizeof(tail));
                    if (prefix) draw_text(rows - 2, 0, prefix, "Cannot read: ");
                    draw_text(rows - 2, prefix, cols - prefix, tail);
                }
            }
            draw_text(rows - 1, 0, cols, keyboard_hint(cols));
            if (editing) {
                int cursor = 8 + text_width(state->filter);
                move(rows - 2, cursor < cols ? cursor : cols - 1);
            }
        }
        refresh();
        int key = getch();
        if (key == KEY_RESIZE || key == ERR) continue;
        if (editing) {
            if (key == '\n' || key == KEY_ENTER || key == 27) {
                if (key == 27) *state = before_edit;
                editing = 0; curs_set(0);
            } else {
                size_t length = strlen(state->filter);
                if (key == KEY_BACKSPACE || key == 127 || key == 8) filter_backspace(state->filter);
                else if (key == 21) state->filter[0] = '\0';
                else if (key >= 32 && key <= 255 && length + 1 < sizeof(state->filter)) {
                    state->filter[length] = (char)key; state->filter[length + 1] = '\0';
                }
            }
            if (editing) state->selected = state->first = 0;
            continue;
        }
        if (key == 'q' || key == 'Q') break;
        if (key == '?' || (help && key == 27)) { help = !help; continue; }
        if (help) continue;
        if (key == '1') view = VIEW_DASHBOARD;
        else if (key == '2') view = VIEW_FOLDERS;
        else if (key == '3') view = VIEW_FILES;
        else if (key == 'l' || key == 'L') view = VIEW_ALL;
        else if (key == 'd' || key == 'D') {
            if (view != VIEW_DUPLICATES) { previous_view = view; view = VIEW_DUPLICATES; }
        } else if (key == 27 && view == VIEW_DUPLICATES) view = previous_view;
        else if (key == 'f' || key == 'F') {
            before_edit = *state;
            editing = 1; curs_set(1);
        } else if ((key == 's' || key == 'S') && view != VIEW_DUPLICATES) {
            sort_mode = (sort_mode + 1) % 3;
            sort_view_states(current, states);
        } else if ((key == KEY_BACKSPACE || key == 127 || key == 8) && view != VIEW_DUPLICATES && current->parent) {
            anchor = (ViewRow){current, SIZE_MAX, 0}; have_anchor = 1;
            current = current->parent;
            memset(states, 0, sizeof(states));
            sort_children(current);
        } else if (items.count) {
            ViewRow item = items.items[state->selected];
            size_t step = 0;
            int backward = 0;
            if (key == KEY_HOME) state->selected = 0;
            else if (key == KEY_END) state->selected = items.count - 1;
            else if (key == KEY_PPAGE) { step = page; backward = 1; }
            else if (key == KEY_NPAGE) step = page;
            else if (key == KEY_UP) { step = cards ? (size_t)columns : 1; backward = 1; }
            else if (key == KEY_DOWN) step = cards ? (size_t)columns : 1;
            else if (key == KEY_LEFT) { step = 1; backward = 1; }
            else if (key == KEY_RIGHT) step = 1;
            else if (key == '\n' || key == KEY_ENTER) {
                if (view == VIEW_DUPLICATES) {
                    expanded = expanded == item.group ? SIZE_MAX : item.group;
                    anchor = (ViewRow){groups[item.group].files[0], item.group, 1}; have_anchor = 1;
                } else if (item.node->is_dir && !item.node->inaccessible) {
                    current = item.node;
                    sort_children(current);
                    memset(states, 0, sizeof(states));
                }
            }
            if (step) {
                if (backward) state->selected = state->selected < step ? 0 : state->selected - step;
                else {
                    size_t remaining = items.count - 1 - state->selected;
                    state->selected += step < remaining ? step : remaining;
                }
            }
        }
    }
    free(items.items); free(reclaimable);
    free_duplicate_groups(groups, group_count);
    endwin(); ui_active = 0;
    save_state(current->path, sort_mode, 0, 0);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("Usage: dupmap [options] [path]\n\n"
               "Options:\n"
               "  --dupes [path]  report duplicate files and reclaimable space\n"
               "  -h, --help      show this help\n"
               "  -v, --version   show version\n\n"
               "Interactive: 1 dashboard, 2 folders, 3 files, d duplicates, l all items.\n"
               "Arrows select, Enter opens/expands, Backspace goes up, Esc returns.\n"
               "f filters names, s cycles sorting, ? shows keyboard help, q quits.\n");
        return EXIT_SUCCESS;
    }
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        puts("dupmap " DUPMAP_VERSION); return EXIT_SUCCESS;
    }
    int dupes_mode = argc > 1 && !strcmp(argv[1], "--dupes");
    char cwd[PATH_MAX], saved_path[PATH_MAX];
    int legacy_color = 0, legacy_list = 0;
    const char *root_path;
    if (dupes_mode) root_path = argc > 2 ? argv[2] : ".";
    else if (argc > 1) root_path = argv[1];
    else if (load_state(saved_path, sizeof(saved_path), &sort_mode, &legacy_color, &legacy_list)) root_path = saved_path;
    else root_path = getcwd(cwd, sizeof(cwd)) ? cwd : ".";
    char root_check[PATH_MAX];
    size_t root_length = strlen(root_path);
    if (root_length < sizeof(root_check)) {
        memcpy(root_check, root_path, root_length + 1);
        while (root_length > 1 && root_check[root_length - 1] == '/') root_check[--root_length] = '\0';
        struct stat root_stat;
        if (lstat(root_check, &root_stat) == 0 && S_ISLNK(root_stat.st_mode)) {
            char visible_path[PATH_MAX]; safe_terminal_text(root_path, visible_path, sizeof(visible_path));
            fprintf(stderr, "dupmap: cannot read '%s': symbolic links are skipped\n", visible_path);
            return EXIT_FAILURE;
        }
    }
    char resolved[PATH_MAX]; if (realpath(root_path, resolved)) root_path = resolved;
    if (!dupes_mode) {
        if (!initscr()) { fprintf(stderr, "dupmap: cannot initialize terminal UI\n"); return EXIT_FAILURE; }
        ui_active = 1; cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
        timeout(-1);
        begin_progress("Scanning folders", root_path);
    }
    Node *root = scan_path(root_path, root_path, 1);
    if (!root) {
        int scan_errno = errno;
        if (ui_active) { endwin(); ui_active = 0; }
        char visible_path[PATH_MAX]; safe_terminal_text(root_path, visible_path, sizeof(visible_path));
        fprintf(stderr, "dupmap: cannot read '%s': %s\n", visible_path, strerror(scan_errno));
        return EXIT_FAILURE;
    }
    if (!dupes_mode && !root->is_dir) {
        endwin(); ui_active = 0; free_node(root);
        char visible_path[PATH_MAX]; safe_terminal_text(root_path, visible_path, sizeof(visible_path));
        fprintf(stderr, "dupmap: '%s' is not a directory\n", visible_path); return EXIT_FAILURE;
    }
    int result = EXIT_SUCCESS;
    if (dupes_mode) {
        DuplicateGroup *groups = NULL;
        size_t group_count = find_duplicate_groups(root, &groups);
        printf("Duplicate groups: %zu\n", group_count);
        for (size_t i = 0; i < group_count; ++i) {
            char size[32]; format_size(duplicate_reclaimable_size(&groups[i]), size, sizeof(size));
            printf("\n%s reclaimable (%zu files):\n", size, groups[i].count);
            for (size_t j = 0; j < groups[i].count; ++j) {
                char visible_path[PATH_MAX];
                safe_terminal_text(groups[i].files[j]->path, visible_path, sizeof(visible_path));
                printf("  %s\n", visible_path);
            }
        }
        free_duplicate_groups(groups, group_count);
    } else result = run_dashboard(root);
    free_node(root);
    return result;
}
