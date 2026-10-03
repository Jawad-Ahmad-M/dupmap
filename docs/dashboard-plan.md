# Monochrome dashboard implementation plan

The user approved the dashboard design and requested implementation in this session.

1. Replace the adaptive treemap and color modes in `src/main.c` with Dashboard,
   Folders, Files, Duplicates, and All items views. Retain scanner safety and
   duplicate accounting. Display real directory entries without synthetic groups.
2. Use fixed 26-column, four-row folder cards, vertically scrollable pages, and
   list fallback when cards cannot fit. Keep per-view selection and filters.
   Render names by terminal-cell width and expose the selected full path below.
3. Add grouped duplicate rows with one expandable group; allow every member to
   be reached by scrolling. Escape restores the previous view.
4. Replace blocking filter entry with an editor that handles resize, Enter,
   Escape, backspace, and Ctrl-U. Keep navigation bounded for empty views.
5. Replace obsolete treemap tests with card viewport and view membership checks.
   Exercise real terminal screens for tabs, scrolling, filtering, folder entry,
   return navigation, duplicate expansion, help, and terminal resize.
6. Update usage documentation, run the full suite and sanitizer checks, and
   inspect the diff before reporting completion.

Review focus: empty folders, inaccessible folders, long and Unicode names,
duplicate groups larger than the viewport, selection after resize/sort, and
filter editing while the terminal shrinks.
