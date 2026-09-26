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
`l` toggles a complete list view, and `q` exits. Very small files are combined into an `other` folder to keep the
treemap readable; press Enter on it to inspect those files. Symlinks are
skipped and inaccessible directories are shown as empty entries rather than
crashing the scan.

The current V1 deliberately focuses on correctness and a dependable core.
Duplicate detection is available through `--dupes`; richer in-treemap duplicate
highlighting is shown with red borders in normal mode. Additional display modes
are planned for the next release.

## Install the man page

```sh
sudo install -Dm644 dupmap.1 /usr/local/share/man/man1/dupmap.1
man dupmap
```
