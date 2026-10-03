#!/bin/sh
set -eu

program=${1:-./dupmap}
fixture=${2:-project_testing}
alpha="$fixture/same-size-different-content/alpha.txt"
beta="$fixture/same-size-different-content/beta.txt"
if [ "$(wc -c <"$alpha")" -ne "$(wc -c <"$beta")" ]; then
    echo "same-size fixture files have different sizes" >&2
    exit 1
fi

help_output=$("$program" --help)
printf '%s\n' "$help_output" | grep -q 'shows keyboard help'
version_output=$("$program" --version)
printf '%s\n' "$version_output" | grep -q '^dupmap '

dupe_output=$("$program" --dupes "$fixture")
printf '%s\n' "$dupe_output" | grep -q '^Duplicate groups: 4$'
printf '%s\n' "$dupe_output" | grep -Fq 'original.txt'
printf '%s\n' "$dupe_output" | grep -Fq 'file with spaces.txt'
printf '%s\n' "$dupe_output" | grep -Fq 'empty-a'
if printf '%s\n' "$dupe_output" | grep -Fq 'same-size-different-content/alpha.txt'; then
    echo "different-content files were incorrectly reported as duplicates" >&2
    exit 1
fi

temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT HUP INT TERM
ln -s "$(cd "$fixture" && pwd)" "$temporary_directory/root-link"
if "$program" "$temporary_directory/root-link/" >"$temporary_directory/out" 2>"$temporary_directory/err"; then
    echo "a symbolic-link root was unexpectedly accepted" >&2
    exit 1
fi
grep -q 'symbolic links are skipped' "$temporary_directory/err"

echo "fixture CLI tests: all passed"
