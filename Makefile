# Makefile for the Paxsy language frontend.
# All C sources under src/ are compiled into a single executable.
# The wildcard source list means a new src/*.c file (such as main.c)
# is picked up automatically on the next build.
# Object files are placed in bin/ to keep the source tree clean; the
# bin/ directory is created on demand.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
TARGET   = paxsy
SRC     := $(wildcard src/*.c)
OBJ     := $(patsubst src/%.c,bin/%.o,$(SRC))

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

bin/%.o: src/%.c | bin
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

bin:
	mkdir -p bin

test: $(TARGET)
	cd tests && python3 test_paxsy.py

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean test
