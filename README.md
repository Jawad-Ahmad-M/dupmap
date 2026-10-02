# dupmap

`dupmap` is a keyboard-driven terminal disk usage viewer written in C. It scans
a directory and displays its contents as a treemap: each box represents a file
or folder, and larger items occupy more space. Enter a folder to explore its
contents, or switch to a full list when the treemap is too crowded.

## Features

- Squarified treemap with size, name, or modification-time sorting.
- Depth, file-type, and size-heat color modes.
- Color-filled treemap tiles, separated gutters, bold folder labels, and a strong selected-item highlight.
- List view with case-insensitive name filtering.
- Exact duplicate file detection, with duplicate entries highlighted in red.
- Remembers the last directory and view preferences when no path is supplied.
- Skips symbolic links and marks folders that cannot be read.

## Requirements and build

Requires a C11 compiler, `make`, and ncurses development headers and library.
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

Run the core and duplicate detection checks with `make test`.

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
| Enter | Open the selected folder |
| Backspace | Go to the parent folder |
| `l` | Toggle treemap/list view |
| `f` | Enter a name filter in list view (empty clears it) |
| `s` | Cycle size, name, and modified-time sorting |
| `c` | Cycle depth, file-type, and size-heat colors |
| `q` | Quit |

Selection briefly pulses as visual feedback when moving through the treemap.
Tiny files are grouped under an `other` folder so they remain accessible while
keeping the treemap readable. The selected item's full path and size appear
below the view; duplicate files have red borders.

## Data and limitations

Scanning is recursive and follows the directory tree while skipping symlinks.
Directory sizes are the sum of visible descendant file sizes, not allocated
disk blocks. Duplicate detection compares file sizes and content hashes, then
confirms matching contents byte-for-byte. It can take time on large trees.
Files smaller than 4 KiB are grouped into `other` folders in the display.

View state is stored in `${XDG_STATE_HOME:-~/.config}/dupmap/state`. State is
optional; inability to save it does not stop the program.

## Install

```sh
sudo make install
```

This installs the executable and `dupmap.1` under `/usr/local` by default.
Set `PREFIX` or `DESTDIR` to change the install location. To install with
CMake, run `sudo cmake --install build-cmake`. See
[CONTRIBUTING.md](CONTRIBUTING.md) for development guidance and
[CHANGELOG.md](CHANGELOG.md) for release history.
