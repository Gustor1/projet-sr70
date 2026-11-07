CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -Isrc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDLIBS = -pthread

.PHONY: all test demo
all: build/banque build/journal build/test_banque

build:
	mkdir -p build

build/banque: src/main.c src/banque.c src/banque.h src/journal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/main.c src/banque.c -o $@ $(LDLIBS)

build/journal: src/journal.c src/journal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/journal.c -o $@ $(LDLIBS)

build/test_banque: tests/test_banque.c src/banque.c src/banque.h src/journal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_banque.c src/banque.c -o $@ $(LDLIBS)

test: all
	./build/test_banque

demo: all
	./build/banque --demo
