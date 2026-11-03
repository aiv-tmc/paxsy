# Makefile for the Paxsy compiler.
# All C sources under src/ (any depth) compile into one executable;
# the module directories carry the architecture (see scripts/
# reorganize.sh). Every object depends on every header: header edits
# rebuild the whole tree, which prevents mixed objects compiled
# against different struct layouts. Object files mirror the source
# tree under bin/; the directories are created at parse time so
# parallel builds never race on mkdir.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
TARGET   = paxsy
SRC     := $(shell find src -name '*.c' -type f | sort)
OBJ     := $(patsubst src/%.c,bin/%.o,$(SRC))
HEADERS := $(shell find src -name '*.h' -type f)
INCS    := -Isrc $(addprefix -I,$(shell find src -mindepth 1 -type d | sort))

# Mirror every source directory into bin/ before any rule runs; with
# all directories in place every compile job is independent and a
# parallel build is race-free.
$(shell find src -mindepth 1 -type d | sed 's|^src|bin|' | xargs -r mkdir -p)

# The built-in assembler and the frame helpers live in nsel.c and are
# declared in nasm.h; a stray nasm.c would collide with them at link
# time, so the build refuses it with a clear message.
ifneq ($(wildcard src/emit/nemit/nasm.c),)
$(error src/emit/nemit/nasm.c must be removed: nasm_stmt lives in nsel.c)
endif

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

bin/%.o: src/%.c $(HEADERS)
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@

# The single test entry point, used both locally and by CI. The
# binary is staged into its own directory with the standard library
# copied beside it: the compiler locates the std sources relative to
# its own executable path (<exe>/lib/std), so the staged layout is a
# faithful miniature of an installed toolchain. The suite runner
# honours PAXSY_CMD.
test: $(TARGET)
	rm -rf /tmp/paxsy-test
	mkdir -p /tmp/paxsy-test
	cp $(TARGET) /tmp/paxsy-test/paxsy
	chmod +x /tmp/paxsy-test/paxsy
	if [ -d lib ]; then cp -r lib /tmp/paxsy-test/lib; fi
	cd tests && PAXSY_CMD=/tmp/paxsy-test/paxsy sh run_all.sh

# AddressSanitizer rebuild of the whole tree plus the full suite.
# Leaves the tree in the normal (non-ASAN) state afterwards so the
# next incremental `make` is a no-op.
test-asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=c99 -O0 -g -fsanitize=address -Wall -Wextra"
	rm -rf /tmp/paxsy-asan
	mkdir -p /tmp/paxsy-asan
	cp $(TARGET) /tmp/paxsy-asan/paxsy
	chmod +x /tmp/paxsy-asan/paxsy
	if [ -d lib ]; then cp -r lib /tmp/paxsy-asan/lib; fi
	cd tests && ASAN_OPTIONS=detect_leaks=0 PAXSY_CMD=/tmp/paxsy-asan/paxsy sh run_all.sh
	$(MAKE) clean
	$(MAKE)

clean:
	rm -rf bin $(TARGET)

.PHONY: all clean test test-asan
