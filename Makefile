CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?=
LDLIBS ?= -lncurses
PREFIX ?= /usr/local
DESTDIR ?=

dupmap: src/main.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

test: tests/test_core.c tests/test_duplicates.c tests/test_layout_dimensions.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o build/test_core $< $(LDLIBS)
	./build/test_core
	$(CC) $(CPPFLAGS) $(CFLAGS) -o build/test_duplicates tests/test_duplicates.c $(LDLIBS)
	./build/test_duplicates
	$(CC) $(CPPFLAGS) $(CFLAGS) -o build/test_layout_dimensions tests/test_layout_dimensions.c $(LDLIBS)
	./build/test_layout_dimensions

install: dupmap
	install -Dm755 dupmap $(DESTDIR)$(PREFIX)/bin/dupmap
	install -Dm644 dupmap.1 $(DESTDIR)$(PREFIX)/share/man/man1/dupmap.1

clean:
	rm -f dupmap build/test_core build/test_duplicates build/test_layout_dimensions

.PHONY: clean test install
