# Test suite

Run `make test` for the full available suite. It includes:

- Core scanner aggregation, parent links, real small-file entries, and nested trees.
- Directory loading helpers, cached node identity, unknown versus empty totals,
  entry counts, and rejection of replaced directory inodes.
- Duplicate groups, byte equality, and same-size non-matches.
- Fixed card dimensions, list fallback, scroll alignment, empty views, and
  selected-item visibility across terminal widths and heights.
- Edge cases for symlink roots, generated-name collisions, arithmetic limits,
  filtering, view membership, expandable duplicate rows, and Unicode clipping.
- Hard-link reclaimable accounting, including links outside the scan.
- CLI smoke checks against the deterministic `project_testing/` fixture.
- POSIX pseudo-terminal checks of rendered screens: monochrome startup, Unicode,
  74 folders, a 70-member duplicate group, tabs, scrolling, folder navigation,
  sorting, filter cancellation, help, and resizing during filter editing.
  Startup totals cover nested files, and navigation uses the startup snapshot.
  These run when Python 3 is installed, with no external Python dependencies.
- On-demand duplicate scanning and reuse of both matching and empty results.
  Linux process I/O counters verify that startup skips duplicate file reads,
  the first D reads candidates, and repeated D reuses the cached result.

`make sanitize` rebuilds and runs the suite with AddressSanitizer and
UndefinedBehaviorSanitizer. CMake exposes the C and fixture checks through
CTest; its pseudo-terminal check is enabled on POSIX when Python 3 is found.
Both build paths require a C11 compiler and wide-character ncurses development headers/library.

The fixture setup script can add a symlink and restrictive permissions for
manual inspection. Restore permissions before deleting the fixture. Permission
tests are not part of the automated suite because privileged users can still
read mode-restricted directories, making that result environment-dependent.

The suite does not claim to prove absence of every defect. It provides stable
regression checks for the listed behavior and should be extended when new
scanner, layout, CLI, or interaction behavior is added.
