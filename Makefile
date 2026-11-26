# Makefile for the Paxsy compiler.
# All C sources under src/ (any depth) compile into one executable.
# Object files mirror the source tree under bin/; compiler-generated
# dependency files track the exact headers used by each translation unit.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -pthread
TARGET   = paxsy
SRC     := $(shell find src -name '*.c' -type f | sort)
OBJ     := $(patsubst src/%.c,bin/%.o,$(SRC))
DEP     := $(OBJ:.o=.d)
INCS    := -Isrc $(addprefix -I,$(shell find src -mindepth 1 -type d | sort))

# Per-source dependency files keep incremental builds precise: editing one
# header recompiles only translation units that actually include it.

# The built-in assembler and the frame helpers live in nsel.c and are
# declared in nasm.h; a stray nasm.c would collide with them at link
# time, so the build refuses it with a clear message.
ifneq ($(wildcard src/emit/nemit/nasm.c),)
$(error src/emit/nemit/nasm.c must be removed: nasm_stmt lives in nsel.c)
endif

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

bin/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

-include $(DEP)

# The single test entry point, used both locally and by CI. The
# binary is staged into its own directory with the standard library
# package copied beside it: the compiler locates the std package
# relative to its own executable path (<exe>/std), so the staged
# layout is a faithful miniature of an installed toolchain. The
# suite runner honours PAXSY_CMD.
test: $(TARGET)
	rm -rf /tmp/paxsy-test
	mkdir -p /tmp/paxsy-test
	cp $(TARGET) /tmp/paxsy-test/paxsy
	chmod +x /tmp/paxsy-test/paxsy
	if [ -d lib/std ]; then cp -r lib/std /tmp/paxsy-test/std; fi
	cd tests && PAXSY_CMD=/tmp/paxsy-test/paxsy sh run_all.sh

# AddressSanitizer build in an isolated directory; the normal object tree
# and executable remain untouched by sanitizer flags.
test-asan:
	rm -rf /tmp/paxsy-asan
	mkdir -p /tmp/paxsy-asan
	$(CC) -std=c99 -O1 -g1 -fno-omit-frame-pointer -fsanitize=address -Wall -Wextra -pthread $(INCS) $(SRC) -o /tmp/paxsy-asan/paxsy
	if [ -d lib/std ]; then cp -r lib/std /tmp/paxsy-asan/std; fi
	env -u MAKEFLAGS -u MAKELEVEL ASAN_OPTIONS=detect_leaks=0 PAXSY_CMD=/tmp/paxsy-asan/paxsy python3 -u tests/run_asan.py

# UndefinedBehaviorSanitizer build in an isolated directory. It runs
# the same compiler and project regression set as the ASan target.
test-ubsan:
	rm -rf /tmp/paxsy-ubsan
	mkdir -p /tmp/paxsy-ubsan
	$(CC) -std=c99 -O1 -g1 -fsanitize=undefined -fno-sanitize-recover=all -Wall -Wextra -pthread $(INCS) $(SRC) -o /tmp/paxsy-ubsan/paxsy
	if [ -d lib/std ]; then cp -r lib/std /tmp/paxsy-ubsan/std; fi
	env -u MAKEFLAGS -u MAKELEVEL UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 PAXSY_CMD=/tmp/paxsy-ubsan/paxsy python3 -u tests/run_asan.py

# Deterministic fuzz suite (grammar + mutation). FUZZ_N scales the
# iteration count; the default is 300+300 programs, seeded, so any
# failure reproduces exactly. Not part of `make test` by design: it
# is the slow, exhaustive gate.
FUZZ_N ?= 300
test-fuzz: $(TARGET)
	rm -rf /tmp/paxsy-fuzz
	mkdir -p /tmp/paxsy-fuzz
	cp $(TARGET) /tmp/paxsy-fuzz/paxsy
	chmod +x /tmp/paxsy-fuzz/paxsy
	if [ -d lib/std ]; then cp -r lib/std /tmp/paxsy-fuzz/std; fi
	cd tests && env -u MAKEFLAGS -u MAKELEVEL FUZZ_N=$(FUZZ_N) PAXSY_CMD=/tmp/paxsy-fuzz/paxsy python3 test_fuzz.py


# End-to-end language/compiler stress benchmark. It builds the same corpus at
# internal no-optimisation reference vs the sole maximum-optimisation mode;
# checks identical output and reports compile/runtime medians.
bench-full: $(TARGET)
	PAXSY_CMD=$(CURDIR)/$(TARGET) python3 benches/bench_full_language.py

clean:
	rm -rf bin $(TARGET)

.PHONY: all clean test test-asan test-ubsan test-fuzz bench-full
