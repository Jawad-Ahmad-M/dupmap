#!/usr/bin/env bash
set -eu

fixture="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

ln -sfn ../duplicates/group-a/original.txt "$fixture/symlinks/link-to-original"
chmod u-rwx "$fixture/permissions/blocked"

printf '%s\n' "Edge cases configured under: $fixture"
printf '%s\n' "- symlink: symlinks/link-to-original"
printf '%s\n' "- unreadable directory: permissions/blocked"
