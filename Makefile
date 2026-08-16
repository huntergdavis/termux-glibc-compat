CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=

WARNINGS := -Wall -Wextra -Werror -Wpedantic -Wcast-qual -Wformat=2 \
	-Wshadow -Wstrict-prototypes
PROBE_NAMES := pthread-basic robust-list sysv-semaphore sysv-shm
PROBES := $(addprefix build/,$(PROBE_NAMES))
CORE_OBJECTS := build/sem_store.o build/protocol.o build/broker_dispatch.o
TESTS := build/test-sem-store build/test-protocol build/test-broker-dispatch

.PHONY: all check clean

all: $(PROBES) $(TESTS)

build:
	mkdir -p $@

build/%: probes/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 -pthread $< $(LDFLAGS) -o $@

build/sem_store.o: src/sem_store.c include/tgcompat/sem_store.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 -c $< -o $@

build/protocol.o: src/protocol.c include/tgcompat/protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 -c $< -o $@

build/broker_dispatch.o: src/broker_dispatch.c include/tgcompat/broker.h \
		include/tgcompat/protocol.h include/tgcompat/sem_store.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 -c $< -o $@

build/test-sem-store: tests/sem-store.c build/sem_store.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

build/test-protocol: tests/protocol.c build/protocol.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

build/test-broker-dispatch: tests/broker-dispatch.c $(CORE_OBJECTS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

check: all
	./scripts/run-probes.sh --no-build
	./build/test-sem-store
	./build/test-protocol
	./build/test-broker-dispatch

clean:
	$(RM) -r build
