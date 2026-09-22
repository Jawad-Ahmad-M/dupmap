# dupmap test fixture

This directory is a deterministic fixture for testing the scanner and duplicate
report. Run dupmap against this directory, not the repository root:

```sh
./dupmap --dupes project_testing
./dupmap project_testing
```

Expected duplicate groups before running the setup script:

- `duplicates/group-a`: three identical files, one reclaimable group
- `duplicates/group-b`: two identical files with spaces in their names
- `duplicates/large`: two identical files larger than the tiny-file threshold
- `same-size-different-content`: no duplicate group
- `empty`: two identical empty files, one duplicate group

Run the optional Linux-only edge-case setup from the repository root:

```sh
bash project_testing/setup_edge_cases.sh
```

That creates a symlink and makes one directory unreadable. It only changes
paths inside `project_testing/` and can be restored with:

```sh
chmod u+rX project_testing/permissions/blocked
rm -f project_testing/symlinks/link-to-original
```
