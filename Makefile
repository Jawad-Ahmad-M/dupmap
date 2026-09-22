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
	$(CC) $(CPPFLAGS) $(CFLAGS) -o build/test_duplicates tests/test_duplicates.c $(LDLIBS)
	./build/test_duplicates

clean:
	rm -f dupmap build/test_core build/test_duplicates

.PHONY: clean test
