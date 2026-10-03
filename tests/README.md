# Test suite

Run `make test` for the full available suite. It includes:

- Core scanner aggregation, parent links, tiny-file grouping, and nested trees.
- Duplicate groups, byte equality, and same-size non-matches.
- Tile bounds, non-overlap, zero-size handling, tight dimensions, and a sweep
  across terminal widths and heights.
- Edge cases for symlink roots, generated-name collisions, arithmetic limits,
  filter matching/wraparound, and 300-item directories.
- CLI smoke checks against the deterministic `project_testing/` fixture.
- POSIX pseudo-terminal startup, help, resize redraw, navigation, sorting,
  filtering, and quit checks at normal, compact, and tiny terminal sizes when
  Python 3 is installed.

`make sanitize` rebuilds and runs the suite with AddressSanitizer and
UndefinedBehaviorSanitizer. CMake exposes the C and fixture checks through
CTest; its pseudo-terminal check is enabled on POSIX when Python 3 is found.
Both build paths require a C11 compiler and ncurses development headers/library.

The fixture setup script can add a symlink and restrictive permissions for
manual inspection. Restore permissions before deleting the fixture. Permission
tests are not part of the automated suite because privileged users can still
read mode-restricted directories, making that result environment-dependent.

The suite does not claim to prove absence of every defect. It provides stable
regression checks for the listed behavior and should be extended when new
scanner, layout, CLI, or interaction behavior is added.
