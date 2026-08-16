CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=

WARNINGS := -Wall -Wextra -Werror -Wpedantic -Wcast-qual -Wformat=2 \
	-Wshadow -Wstrict-prototypes
PROBE_NAMES := pthread-basic robust-list sysv-semaphore sysv-shm
PROBES := $(addprefix build/,$(PROBE_NAMES))
CORE_OBJECTS := build/sem_store.o
TESTS := build/test-sem-store

.PHONY: all check clean

all: $(PROBES) $(TESTS)

build:
	mkdir -p $@

build/%: probes/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 -pthread $< $(LDFLAGS) -o $@

build/sem_store.o: src/sem_store.c include/tgcompat/sem_store.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 -c $< -o $@

build/test-sem-store: tests/sem-store.c $(CORE_OBJECTS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

check: all
	./scripts/run-probes.sh --no-build
	./build/test-sem-store

clean:
	$(RM) -r build
