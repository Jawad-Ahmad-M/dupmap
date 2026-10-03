# dupmap

`dupmap` is a keyboard-driven terminal disk usage viewer written in C. It scans
a directory and opens a monochrome dashboard with scrollable folder cards,
separate file listings, and a dedicated duplicate browser.

## Features

- Fixed-size folder cards with vertical scrolling and a list fallback in small terminals.
- Dashboard summaries for folder size, folder count, direct files, and scan-wide duplicates.
- Files and All items lists with name filtering and size, name, or modified-time sorting.
- Exact duplicate groups with expandable, scrollable paths and reclaimable space totals.
- Black-and-white rendering with reverse-video selection and Unicode name support.
- Remembers the last directory and sort preference when no path is supplied.
- Skips symbolic links and marks folders that cannot be read.

## Requirements and build

Requires a C11 compiler, `make`, and wide-character ncurses development headers and library.
On Debian, Ubuntu, or WSL:

```sh
sudo apt install build-essential libncurses-dev
make
```

The executable is created as `./dupmap`. CMake is also supported:

```sh
cmake -S . -B build-cmake
cmake --build build-cmake
```

Run the scanner, duplicate, dashboard, edge-case, and fixture CLI checks with
`make test`. Run the same CTest suite with
`ctest --test-dir build-cmake --output-on-failure`. On GCC or Clang, `make sanitize` runs the checks with
AddressSanitizer and UndefinedBehaviorSanitizer enabled.

For hands-on visual checks, run `make lab` or `bash project_testing/run.sh`.
It opens generated edge-case datasets in the TUI and gives you a checklist for
each one. See [`project_testing/README.md`](project_testing/README.md).

## Use

```sh
./dupmap [path]
./dupmap --dupes [path]
```

With no path, dupmap opens the last saved directory when it is available;
otherwise it scans the current directory. `--dupes` prints duplicate groups
and estimated reclaimable space, then exits without starting the TUI.

### Interactive keys

| Key | Action |
| --- | --- |
| Arrow keys | Select an item |
| Page Up / Page Down | Move one visible page at a time |
| Home / End | Select the first or last entry |
| Enter | Open a folder or expand/collapse a duplicate group |
| Backspace | Go to the parent folder |
| `1` / `2` / `3` | Dashboard / Folders / Files |
| `d` | Open scan-wide duplicate groups |
| Escape | Return from duplicates; cancel filter editing; close help |
| `l` | Open All items, showing direct folders and files together |
| `f` | Edit the current view's filter; Enter applies, Escape cancels, Ctrl-U clears |
| `s` | Cycle size, name, and modified-time sorting |
| `?` | Show keyboard help (`?` or Escape closes it) |
| `q` | Quit |

Dashboard and Folders show only real folders, including empty and unreadable
folders. Every card stays 26 columns wide and four rows high; terminal size
changes the number of visible cards. Unreadable folders use a `[D!]` marker
in lists and a message in cards; Enter does not open them. Additional cards
scroll vertically.
When a card cannot fit, the same entries appear as a list.

Files shows files directly inside the current folder, including small and empty
files. All items preserves navigation through the full filesystem structure.
Each view keeps its selection and filter when switching tabs. Entering another
folder resets its view filters. Long names are clipped by terminal-cell width;
the selected path's ending is shown in the status line.

Duplicates covers the entire scanned tree. Enter expands a group; scroll its
member paths with arrows, paging, or Home/End. File contents are checked only
when `d` is first pressed. Startup scans the full directory tree using names and
metadata, so folder sizes are available immediately in the dashboard. File
contents are not read at startup. A loading screen shows the current phase,
path, entry count, elapsed time, and activity indicator; press Q to quit during
scanning. Unreadable folders have incomplete totals. Duplicate groups and reclaimable totals are cached
for the session, including a scan with no matches. Before scanning, the dashboard
shows "not checked". The `--dupes` command checks immediately.
Enter on a member collapses its
group. Escape restores the preceding view and selection.

## Data and limitations

The startup scan traverses the full directory tree while skipping symlinks.
Ordinary browsing uses the cached directory tree.
Directory sizes are the sum of visible descendant file sizes, not allocated
disk blocks. Duplicate detection compares file sizes and content hashes, then
confirms matching contents byte-for-byte. It can take time on large trees.
Reclaimable space counts distinct file inodes, keeps one copy, and excludes
data retained by hard links outside the duplicate group. These totals use the
scanned logical sizes; filesystem changes after scanning can invalidate them.
Files remain in their real folders; no synthetic grouping is applied.

View state is stored in `${XDG_STATE_HOME:-~/.config}/dupmap/state`. State is
optional; inability to save it does not stop the program.

See [`docs/architecture.md`](docs/architecture.md) for the source and data-flow
overview and [`docs/security.md`](docs/security.md) for the security review and
operational limitations.

## Install

```sh
sudo make install
```

This installs the executable and `dupmap.1` under `/usr/local` by default.
Set `PREFIX` or `DESTDIR` to change the install location. To install with
CMake, run `sudo cmake --install build-cmake`. See
[CONTRIBUTING.md](CONTRIBUTING.md) for development guidance and
[CHANGELOG.md](CHANGELOG.md) for release history.
