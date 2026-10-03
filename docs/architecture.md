# Architecture

`dupmap` is a single-file C11 application. `src/main.c` contains the filesystem
model, layout, terminal rendering, keyboard handling, and command-line entry
points. The test programs include that file directly so they can exercise its
static helpers without a separate library API.

## Data flow

1. Parse `--help`, `--version`, `--dupes`, or the optional scan root.
2. For interactive mode, initialize ncurses and report scan progress.
3. Recursively build a tree of `Node` values. Each node owns its name, path,
   and child vector; directory size and file count are aggregated from children
   with saturation at the representable limits.
4. Sort each directory by the active size, name, or modification-time mode.
5. For duplicate detection, collect regular files, bucket by size, hash only
   buckets with repeated sizes, and byte-compare candidates that also share a
   hash. Hashes are a prefilter, never proof of equality.
6. Convert visible children into terminal-cell rectangles. Squarified layout is
   used for ordinary groups; a grid fallback caps work for very large groups.
   If tiles would become too small, render a scrollable list instead.
7. Redraw after each input or resize, then free the duplicate groups and tree.

## Ownership and invariants

- A `Node` owns all its children, its name, and its full path. Parent pointers
  are non-owning. `free_node` recursively releases the tree.
- A `DuplicateGroup` owns its pointer array but refers to `Node` objects owned
  by the tree. Release the groups before releasing the tree.
- `BoxList` owns its dynamic array; each box only refers to a tree node.
- Arrays grow through `grow_array`, which checks capacity and multiplication
  overflow before reallocating.
- Layout coordinates are terminal cells. Emitted rectangles have positive
  dimensions, remain within the requested viewport, and do not overlap.

## Performance notes

Tree construction and traversal are linear in the number of entries. Directory
children are sorted once after scanning. Duplicate detection sorts all file
records by size, hashes only files in repeated-size buckets, and sorts those
records by hash before exact comparisons. Typical work is O(n log n) plus file
reads; pathological same-size/hash collisions can still require quadratic
byte comparisons. Treemap layout falls back to a bounded grid when there are
more than 256 positive-size children.

The UI is intentionally synchronous: scanning and duplicate checks happen on
the UI thread, with periodic progress redraws. Large or slow filesystems can
therefore delay keyboard input until the current operation reaches its next
progress update or completes.
