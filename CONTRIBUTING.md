# Contributing to dupmap

## Development setup

On Ubuntu or WSL:

```sh
sudo apt update
sudo apt install -y build-essential libncurses-dev cmake
git clone https://github.com/Jawad-Ahmad-M/dupmap.git
cd dupmap
```

Run the checks before opening a pull request:

```sh
make test
make
cmake -S . -B build-cmake
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

Please add or update a test when changing scanner, duplicate, layout, or
filesystem behavior. Keep platform-specific behavior graceful: symlinks must
not be followed, unreadable directories must not crash the scan, and duplicate
matches must be byte-verified after hashing.
