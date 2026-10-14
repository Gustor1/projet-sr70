CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -Isrc
LDLIBS = -pthread

.PHONY: all test demo
all: build/banque build/test_banque

build:
	mkdir -p build

build/banque: src/main.c src/banque.c src/banque.h src/synchronisation.h | build
	$(CC) $(CFLAGS) src/main.c src/banque.c -o $@ $(LDLIBS)

build/test_banque: tests/test_banque.c src/banque.c src/banque.h src/synchronisation.h | build
	$(CC) $(CFLAGS) tests/test_banque.c src/banque.c -o $@ $(LDLIBS)

test: all
	./build/test_banque

demo: all
	./build/banque --demo
