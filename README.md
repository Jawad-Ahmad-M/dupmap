# dupmap

Version 0.1.0

`dupmap` is a small terminal disk-usage visualizer. It scans a directory, shows
its contents as a treemap, and lets you navigate into folders without leaving
the terminal.

The treemap uses bordered, color-coded boxes with a clear selected-box state.
Use the list view whenever a directory contains more items than can fit
readably in the available terminal space.

## Build

On Debian/Ubuntu/WSL:

```sh
sudo apt install build-essential libncurses-dev
make
```

Run the automated core tests with:

```sh
make test
```

## Run

```sh
./dupmap [path]
```

To list duplicate files and estimated reclaimable space:

```sh
./dupmap --dupes [path]
```

Keys: arrows select an item, `Enter` opens a directory, `Backspace` goes up,
`l` toggles a complete list view, `f` filters names in the list view, `s` cycles size/name/modified sorting, `c`
cycles depth/file-type/size-heat colors, and `q` exits. Very small files are combined into an `other` folder to keep the
treemap readable; press Enter on it to inspect those files. Symlinks are
skipped and inaccessible directories are shown as empty entries rather than
crashing the scan.

The current V1 deliberately focuses on correctness and a dependable core.
Duplicate detection is available through `--dupes`; duplicate files are also
highlighted with red borders in normal mode.

## Install the man page

```sh
sudo install -Dm644 dupmap.1 /usr/local/share/man/man1/dupmap.1
man dupmap
```

To install the program and man page together:

```sh
sudo make install
```

The project also supports CMake:

```sh
cmake -S . -B build-cmake
cmake --build build-cmake
sudo cmake --install build-cmake
```

When launched without an explicit path, dupmap remembers the last directory,
sort mode, color mode, and view mode in `${XDG_STATE_HOME:-~/.config}/dupmap/state`.
The state file is optional and failures to write it do not affect scanning.

## Release

Linux release archives are built automatically when a `v*` tag is pushed. To
publish the current version from a maintainer checkout:

```sh
git tag v0.1.0
git push origin v0.1.0
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full test and development loop.
