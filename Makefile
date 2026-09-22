CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?=
LDLIBS ?= -lncurses

dupmap: src/main.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

test: tests/test_core.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o build/test_core $< $(LDLIBS)
	./build/test_core

clean:
	rm -f dupmap build/test_core

.PHONY: clean test
