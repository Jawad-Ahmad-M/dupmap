#!/usr/bin/env bash
set -eu

repo_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
binary="${1:-$repo_dir/dupmap}"
if [ ! -x "$binary" ]; then
    printf 'Build dupmap first with: make\nExpected executable: %s\n' "$binary" >&2
    exit 1
fi

make_case() {
    case_name=$1
    case_dir="$scratch/$case_name"
    mkdir -p "$case_dir"
    case "$case_name" in
        empty)
            mkdir -p "$case_dir/empty-folder"
            ;;
        tiny-boundaries)
            for size in 0 1 4095 4096 4097; do
                head -c "$size" /dev/zero >"$case_dir/${size}-bytes.bin"
            done
            ;;
        generated-name-collision)
            head -c 3 /dev/zero >"$case_dir/other"
            mkdir "$case_dir/other (2)"
            head -c 5000 /dev/zero >"$case_dir/other (2)/large.bin"
            head -c 5000 /dev/zero >"$case_dir/large.bin"
            ;;
        crowded)
            for n in $(seq 1 300); do
                size=$(( (n * 137) % 12000 ))
                head -c "$size" /dev/zero >"$case_dir/item-$n.bin"
            done
            ;;
        duplicates)
            mkdir "$case_dir/group" "$case_dir/same-size-different-content"
            printf 'matching payload\n' >"$case_dir/group/one.txt"
            cp "$case_dir/group/one.txt" "$case_dir/group/two copy.txt"
            : >"$case_dir/empty-a"
            : >"$case_dir/empty-b"
            printf 'alpha' >"$case_dir/same-size-different-content/alpha.txt"
            printf 'bravo' >"$case_dir/same-size-different-content/bravo.txt"
            ;;
        deep-tree)
            deep="$case_dir"
            for n in $(seq 1 24); do
                deep="$deep/level-$n"
                mkdir "$deep"
            done
            printf 'deep file\n' >"$deep/deep file.txt"
            printf 'sibling\n' >"$case_dir/top-level.txt"
            ;;
        names)
            mkdir "$case_dir/folder with spaces"
            unicode_name=$(printf 'caf\u00e9-\u6771\u4eac.txt')
            printf 'unicode\n' >"$case_dir/folder with spaces/$unicode_name"
            long_name=$(printf '%180s' '' | tr ' ' x)
            printf 'long name\n' >"$case_dir/$long_name.txt"
            ;;
        *)
            printf 'Unknown case: %s\n' "$case_name" >&2
            return 1
            ;;
    esac
}

while :; do
    printf '\n dupmap manual testing lab\n'
    printf ' 1) Empty folder\n 2) Tiny-file size boundaries\n 3) Generated-name collision\n'
    printf ' 4) Crowded directory (300 entries)\n 5) Duplicate and near-match files\n'
    printf ' 6) Deep directory tree\n 7) Spaces, Unicode, and long filename\n q) Quit\n'
    printf 'Choose a case: '
    IFS= read -r choice || exit 0
    case "$choice" in
        1) name=empty; hint='The empty folder has no file tiles. Check that the screen still explains the empty state.' ;;
        2) name=tiny-boundaries; hint='Press 3: every file remains directly in Files, including empty and small files.' ;;
        3) name=generated-name-collision; hint='The real file other and directory other (2) remain in Files and Folders; L shows both.' ;;
        4) name=crowded; hint='Press 3 to browse 300 files. Check paging, Home/End, filtering, and resizing.' ;;
        5) name=duplicates; hint='Press D and expand groups. Identical and empty files belong together; alpha/bravo do not.' ;;
        6) name=deep-tree; hint='Use Enter to descend and Backspace to return. Check path clipping and selection across levels.' ;;
        7) name=names; hint='Check that Unicode and long names clip cleanly and remain selectable.' ;;
        q|Q) exit 0 ;;
        *) printf 'Choose 1–7 or q.\n'; continue ;;
    esac

    scratch=$(mktemp -d "${TMPDIR:-/tmp}/dupmap-lab.XXXXXX")
    trap 'rm -rf "$scratch"' EXIT HUP INT TERM
    make_case "$name"
    printf '\nCase: %s\nWhat to inspect: %s\n' "$name" "$hint"
    printf 'Resize the terminal while dupmap is open to check responsive layout.\n'
    printf 'Press Enter to launch, or Ctrl-C to leave the lab: '
    IFS= read -r _ || exit 0
    if "$binary" "$case_dir"; then
        :
    else
        result=$?
        printf 'dupmap exited with status %s for case %s.\n' "$result" "$name"
    fi
    rm -rf "$scratch"
    trap - EXIT HUP INT TERM
done
