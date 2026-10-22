CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -Isrc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDLIBS = -pthread

.PHONY: all test test-systeme demo
all: build/banque build/test_banque

build:
	mkdir -p build

build/banque: src/main.c src/banque.c src/banque.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/main.c src/banque.c -o $@ $(LDLIBS)

build/test_banque: tests/test_banque.c src/banque.c src/banque.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_banque.c src/banque.c -o $@ $(LDLIBS)

test: all
	./build/test_banque

test-systeme: all
	python3 tests/test_systeme.py

demo: all
	./build/banque --demo
