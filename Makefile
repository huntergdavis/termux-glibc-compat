CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=

WARNINGS := -Wall -Wextra -Werror -Wpedantic -Wcast-qual -Wformat=2 \
	-Wshadow -Wstrict-prototypes
PROBE_NAMES := pthread-basic robust-list sysv-semaphore sysv-shm
PROBES := $(addprefix build/,$(PROBE_NAMES))
CORE_OBJECTS := build/sem_store.o build/protocol.o build/broker_dispatch.o
SERVER_OBJECTS := build/transport.o build/broker_server.o
TESTS := build/test-sem-store build/test-protocol build/test-broker-dispatch \
	build/test-transport build/test-transport-security \
	build/test-broker-integration

.PHONY: all check clean

all: $(PROBES) build/tgcompatd $(TESTS)

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

build/transport.o: src/transport.c include/tgcompat/transport.h \
		include/tgcompat/protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 -c $< -o $@

build/broker_server.o: src/broker_server.c include/tgcompat/broker.h \
		include/tgcompat/transport.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 -c $< -o $@

build/tgcompatd: src/tgcompatd.c $(CORE_OBJECTS) $(SERVER_OBJECTS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

build/test-sem-store: tests/sem-store.c build/sem_store.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

build/test-protocol: tests/protocol.c build/protocol.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

build/test-broker-dispatch: tests/broker-dispatch.c $(CORE_OBJECTS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

build/test-transport: tests/transport.c build/protocol.o build/transport.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

build/test-transport-security: tests/transport-security.c build/protocol.o \
		build/transport.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

build/test-broker-integration: tests/broker-integration.c build/protocol.o \
		build/transport.o build/tgcompatd | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 \
		tests/broker-integration.c build/protocol.o build/transport.o \
		$(LDFLAGS) -o $@

check: all
	./scripts/run-probes.sh --no-build
	./build/test-sem-store
	./build/test-protocol
	./build/test-broker-dispatch
	./build/test-transport
	./build/test-transport-security
	./build/test-broker-integration

clean:
	$(RM) -r build
