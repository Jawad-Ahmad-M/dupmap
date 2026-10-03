# dupmap testing lab

This folder is for hands-on visual checks. It includes fixed sample data and an
interactive runner that builds one isolated scenario at a time, opens it in
dupmap, and tells you what to inspect. The temporary data is removed when you
exit the app.

From the repository root, build and run the lab:

```sh
make
bash project_testing/run.sh
```

Use arrow keys to move, Enter to open folders, Backspace to go up, `l` to
switch between treemap and list views, and `q` to close dupmap. Press `?` for
the full key guide. In list view, try filtering with `f`; try sorting with `s`
and color modes with `c` where supported. While a case is open, resize the
terminal window in both directions. Try a small terminal, then a normal or
wide one; the layout should adapt without losing selection or clipping tiles
outside the view. Press Enter after quitting dupmap to choose another case.
All generated files live in a temporary directory.

## Manual scenarios

| Runner choice | Checks | What should happen |
| --- | --- | --- |
| Empty folder | Empty-state view | No crash or stray tiles; navigation and quit still work. |
| Tiny-file boundaries | 0, 1, 4095, 4096, and 4097 byte files | Files below 4096 bytes appear inside a synthetic `other` folder; 4096 and 4097 byte files remain separate. |
| Generated-name collision | Tiny file `other`, real directory `other (2)` | Both real entries stay intact; the synthetic tiny-file folder is named `other (3)`. |
| Crowded directory | 300 entries with varied sizes | It should open in a scrollable list. Check the visible range, arrow scrolling, Page Up/Down, Home/End, then press `l` to compare tiles and resize the terminal. |
| Duplicate and near-match files | Identical content, empty duplicates, same-size different content | Identical files are marked; equal-size files with different bytes are not. Enter folders to inspect grouped tiny files. |
| Deep directory tree | 24 nested folders and a spaced filename | Enter/Backspace navigation works and the path remains readable. |
| Spaces, Unicode, and long filename | `café-東京.txt`, spaces, 180-character name | Names clip cleanly without corrupting the display or breaking selection. |

The runner creates each scenario from scratch, so cases do not affect each
other. If you want to inspect fixed integration data instead, run:

```sh
./dupmap project_testing
./dupmap --dupes project_testing
```

Fixed fixture groups include duplicate files, empty files, same-size different
content, nested paths, Unicode, and tiny files. Optional Linux-only symlink
and unreadable-directory fixtures can be enabled with
`bash project_testing/setup_edge_cases.sh`; restore them as described in that
script before deleting the fixture.

## Automated checks

The manual lab complements the regression suite; it does not replace it. From
the repository root run:

```sh
make test
make sanitize
```

The suite covers scanner aggregation, duplicate detection, layout bounds and
terminal-size sweeps, filesystem edge cases, fixture CLI behavior, and
pseudo-terminal navigation/resize/filter/sort/quit behavior. See
[`../tests/README.md`](../tests/README.md) for the automated coverage details.
