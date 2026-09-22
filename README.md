# dupmap

`dupmap` is a small terminal disk-usage visualizer. It scans a directory, shows
its contents as a treemap, and lets you navigate into folders without leaving
the terminal.

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
and `q` exits. Very small files are combined into an `other` folder to keep the
treemap readable; press Enter on it to inspect those files. Symlinks are
skipped and inaccessible directories are shown as empty entries rather than
crashing the scan.

The current V1 deliberately focuses on correctness and a dependable core.
Duplicate detection and richer visualization are planned for V2.
