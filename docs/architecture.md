# Architecture

`dupmap` is a single-file C11 application. `src/main.c` contains the filesystem
model, layout, terminal rendering, keyboard handling, and command-line entry
points. The test programs include that file directly so they can exercise its
static helpers without a separate library API.

## Data flow

1. Parse `--help`, `--version`, `--dupes`, or the optional scan root.
2. For interactive mode, initialize ncurses and report scan progress.
3. Scan the full directory tree using metadata only. Nodes retain their addresses.
   Totals saturate at representable limits and remain unknown for unreadable trees.
4. Sort each directory by the active size, name, or modification-time mode.
5. On the first interactive D press (or immediately for `--dupes`),
   collect regular files, bucket by size, hash only
   buckets with repeated sizes, and byte-compare candidates that also share a
   hash. Hashes are a prefilter, never proof of equality. Cache even empty results
   for the session. Startup and ordinary browsing do not read file contents.
6. Build view rows from the real directory entries or scan-wide duplicate groups.
   Dashboard and Folders use fixed 26-by-4 cards with vertically scrollable rows.
   If a card cannot fit, use a list. Files and All items use lists; expanded
   duplicate groups add independently scrollable member rows.
7. Redraw after each input or resize, then free the duplicate groups and tree.

## Ownership and invariants

- A `Node` owns all its children, its name, and its full path. Parent pointers
  are non-owning. `free_node` recursively releases the tree.
- A `DuplicateGroup` owns its pointer array but refers to `Node` objects owned
  by the tree. Release the groups before releasing the tree.
- `ViewRows` owns its dynamic array; rows refer to tree nodes and group indices.
- Each view has independent selection, viewport, and filter state. Directory
  navigation resets filters; duplicate browsing keeps the directory unchanged.
- Arrays grow through `grow_array`, which checks capacity and multiplication
  overflow before reallocating.
- Card dimensions are fixed terminal-cell counts. The selected row stays inside
  the viewport after input, sorting, and resizing. Wide-character ncurses and
  terminal-cell clipping keep Unicode labels within their allotted columns.

## Performance notes

Startup visits the full tree without reading file contents; navigation uses the
cached tree. Metadata traversal is linear in the number of entries.
Directory children are sorted after loading and when navigation or sorting requests it. Duplicate detection sorts all file
records by size, hashes only files in repeated-size buckets, and sorts those
records by hash before exact comparisons. Typical work is O(n log n) plus file
reads; pathological same-size/hash collisions can still require quadratic
byte comparisons. Reclaimable totals are cached for the UI after accounting
for inode identity and hard links outside each group. View rows are rebuilt
from the current entries; rendering visits only the visible page.

The UI is intentionally synchronous: metadata scanning and requested duplicate checks happen on
the UI thread, with loading redraws limited to ten per second. The loading screen
shows phase, path, entries, elapsed time, and an activity indicator. Progress
updates poll Q to quit and handle resizing, including during hashing and byte
comparison of large files. A blocked filesystem call can delay the next update.
