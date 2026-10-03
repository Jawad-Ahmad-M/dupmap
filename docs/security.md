# Security review

This document describes the security properties of the current implementation;
it is not a claim that every possible defect has been ruled out.

## Filesystem handling

- Scans use `lstat` and skip symbolic links and non-regular, non-directory
  entries. Opening a directory uses `O_NOFOLLOW`, `O_DIRECTORY`, and an
  `fstat` device/inode comparison so a final-component symlink or swapped
  directory is not followed.
- File hashing and byte comparison open with `O_NOFOLLOW` and nonblocking
  mode, then verify with `fstat` that the opened object is a regular file.
  FIFOs, devices, sockets, and symlinks cannot make duplicate detection block
  or read arbitrary device data.
- A scan is read-only. It does not delete, rename, or modify scanned files.
  Directories can change during a scan; the result is a best-effort snapshot,
  and intermediate path components are not held open for the entire walk.
- Duplicate matching verifies contents byte-for-byte after a 64-bit FNV-1a
  prefilter. The hash is not cryptographic and is not used as proof of equality.
- Duplicate reclaimable-space output is an estimate based on path names and
  apparent file sizes. Multiple names for the same hard-linked inode can make
  that estimate larger than the space actually recoverable. The program never
  removes duplicates automatically.

## Terminal and saved state

- File and directory names are untrusted input. Control characters and invalid
  UTF-8 are replaced before terminal rendering or CLI path output, reducing the
  risk of ANSI escape or malformed text injection.
- Saved preferences are optional. The app uses an absolute state location,
  checks that its app directory is owned by the current user and not writable
  by group or others, opens the state file without following a symlink, checks
  ownership and regular-file type, and sets mode `0600`. Bad state is ignored.
- The app does not invoke a shell or execute paths found during a scan.

## Build and release workflows

- CI has read-only `contents` permission. The release workflow grants only
  `contents: write`, which is needed to publish a release.
- Third-party actions are pinned to full commit IDs. CI runs on pushes and pull
  requests, installs the system ncurses toolchain, runs the test suite, and
  builds the program.
- Release packaging currently produces a Linux x86-64 archive. Verify the
  archive and tag before publishing a release.

## Remaining limits

- The scanner is synchronous and can consume substantial time, memory, open
  calls, and stack depth on very large or deeply nested directory trees.
- Files can be replaced or edited while scanning. No destructive operation
  follows from the resulting snapshot, but concurrent changes can make sizes
  and contents inconsistent across separate reads.
- `O_NOFOLLOW` is required at compile time. This is a POSIX/Linux-oriented
  ncurses program and does not promise support for platforms without it.
- Automated checks cover specified regressions, not a formal proof, fuzzing,
  or a complete security certification. Run `make test` and `make sanitize`
  using a supported POSIX environment before distributing changes.
