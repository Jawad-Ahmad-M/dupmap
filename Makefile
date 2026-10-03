CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?=
LDLIBS ?= -lncurses
PREFIX ?= /usr/local
DESTDIR ?=

dupmap: src/main.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

test: tests/test_core.c tests/test_duplicates.c tests/test_layout_dimensions.c tests/test_edge_cases.c dupmap
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o build/test_core $< $(LDLIBS)
	./build/test_core
	$(CC) $(CPPFLAGS) $(CFLAGS) -o build/test_duplicates tests/test_duplicates.c $(LDLIBS)
	./build/test_duplicates
	$(CC) $(CPPFLAGS) $(CFLAGS) -o build/test_layout_dimensions tests/test_layout_dimensions.c $(LDLIBS)
	./build/test_layout_dimensions
	$(CC) $(CPPFLAGS) $(CFLAGS) -o build/test_edge_cases tests/test_edge_cases.c $(LDLIBS)
	./build/test_edge_cases
	sh tests/test_fixture_cli.sh ./dupmap project_testing
	@if command -v python3 >/dev/null 2>&1; then case "$$(uname -s)" in MINGW*|MSYS*|CYGWIN*) echo "Skipping pseudo-terminal UI checks (POSIX PTY required)";; *) python3 tests/test_tui_pty.py ./dupmap project_testing;; esac; else echo "Skipping pseudo-terminal UI checks (Python 3 required)"; fi

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" test

install: dupmap
	install -Dm755 dupmap $(DESTDIR)$(PREFIX)/bin/dupmap
	install -Dm644 dupmap.1 $(DESTDIR)$(PREFIX)/share/man/man1/dupmap.1

clean:
	rm -f dupmap build/test_core build/test_duplicates build/test_layout_dimensions build/test_edge_cases

.PHONY: clean test sanitize install
