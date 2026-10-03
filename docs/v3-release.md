# Dupmap V3 is complete!

After V2 added duplicate detection and disk analysis features, V3 focused on
making dupmap easier to navigate and more predictable to use.

What's new:

- A monochrome dashboard with separate Folders, Files, Duplicates, and All items views.
- Fixed-size folder cards with scrolling and a list fallback for small terminals.
- On-demand duplicate checking: press D to scan file contents, then reuse cached results.
- Expandable duplicate groups with scrollable file paths and reclaimable space estimates.
- A loading screen with an activity indicator, current path, entry count, elapsed time, and Q to quit.
- Full metadata scanning at startup, so folder sizes are ready when the dashboard opens.
- Improved filtering, Unicode rendering, resizing, and selection handling.
- Files stay in their real folders, including small files.
- Expanded automated terminal and filesystem regression coverage.

Build and run:

```sh
make
./dupmap [path]
./dupmap --dupes [path]
```

This phase deepened my understanding of terminal UI design, filesystem scanning,
performance trade-offs, and regression testing in C.

Next, I'd like to improve packaging and make dupmap easier to install system-wide.
Suggestions on usability, performance, or packaging are welcome!

GitHub repository: https://lnkd.in/d8C8PmKC

#C #Linux #WSL #OpenSource #Ncurses #TerminalTools #SystemsProgramming
