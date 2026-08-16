CC ?= cc
GLIBC_CC ?= $(if $(filter Android,$(shell uname -o 2>/dev/null)),grun -s gcc,$(CC))
AR ?= ar
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
STRIP ?= strip
BASH ?= bash
PROBE_SHM_LIBS ?= $(if $(filter Android,$(shell uname -o 2>/dev/null)),\
	-landroid-shmem,)
PROBE_SEM_LIBS ?= $(if $(filter Android,$(shell uname -o 2>/dev/null)),\
	-landroid-sysv-semaphore,)

RELEASE_CFLAGS ?= -O3 -DNDEBUG -flto -fno-plt \
	-fno-semantic-interposition -fomit-frame-pointer \
	-ffunction-sections -fdata-sections
RELEASE_CPU_FLAGS ?=
RELEASE_LDFLAGS ?= -flto -Wl,-O2,--as-needed,--gc-sections \
	-Wl,-z,relro,-z,now
EXEC_SHIM_CFLAGS ?= -O3 -DNDEBUG -fPIC -fvisibility=hidden \
	-fno-semantic-interposition -ffunction-sections -fdata-sections
EXEC_SHIM_LDFLAGS ?= -shared -Wl,-O2,--as-needed,--gc-sections \
	-Wl,-z,relro,-z,now
ANDROID_ROOT_SHIM_CFLAGS ?= -O3 -DNDEBUG -fPIC -fvisibility=hidden \
	-fno-semantic-interposition -ffunction-sections -fdata-sections
ANDROID_ROOT_SHIM_LDFLAGS ?= -shared -Wl,-O2,--as-needed,--gc-sections \
	-Wl,-z,relro,-z,now

WARNINGS := -Wall -Wextra -Werror -Wpedantic -Wcast-qual -Wformat=2 \
	-Wshadow -Wstrict-prototypes
THREAD_FLAGS := -pthread
PROBE_NAMES := pthread-basic robust-list sysv-semaphore sysv-shm
PROBES := $(addprefix build/,$(PROBE_NAMES))
CORE_OBJECTS := build/sem_store.o build/protocol.o build/broker_dispatch.o
SERVER_OBJECTS := build/transport.o build/broker_server.o
CLIENT_OBJECTS := build/protocol.o build/transport.o build/client.o
TESTS := build/test-sem-store build/test-protocol build/test-broker-dispatch \
	build/test-transport build/test-transport-security \
	build/test-broker-integration build/test-client

.PHONY: all benchmark check check-broker check-exec-shim clean exec-shim \
	release

all: $(PROBES) build/tgcompatd build/libtgcompat-client.a \
	build/libtgcompat-exec.so build/libtgcompat-android-root.so \
	build/test-android-root-shim $(TESTS)

build:
	mkdir -p $@

build/%: probes/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 -pthread $< $(LDFLAGS) -o $@

build/sysv-shm: probes/sysv-shm.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 -pthread \
		$< $(LDFLAGS) $(PROBE_SHM_LIBS) -o $@

build/sysv-semaphore: probes/sysv-semaphore.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 -pthread \
		$< $(LDFLAGS) $(PROBE_SEM_LIBS) -o $@

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

build/client.o: src/client.c include/tgcompat/client.h \
		include/tgcompat/protocol.h include/tgcompat/sem_store.h \
		include/tgcompat/transport.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 -c $< -o $@

build/libtgcompat-client.a: $(CLIENT_OBJECTS) | build
	$(AR) rcs $@ $^

build/broker_server.o: src/broker_server.c include/tgcompat/broker.h \
		include/tgcompat/transport.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 \
		$(THREAD_FLAGS) -c $< -o $@

build/tgcompatd: src/tgcompatd.c $(CORE_OBJECTS) $(SERVER_OBJECTS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 \
		$(THREAD_FLAGS) $^ $(LDFLAGS) -o $@

build/libtgcompat-exec.so: src/exec_shim.c | build
	$(GLIBC_CC) $(CPPFLAGS) $(EXEC_SHIM_CFLAGS) $(WARNINGS) -std=c11 \
		$< $(EXEC_SHIM_LDFLAGS) -ldl -o $@

build/libtgcompat-android-root.so: src/android_root_shim.c \
		include/tgcompat/android_root.h | build
	$(GLIBC_CC) $(CPPFLAGS) $(ANDROID_ROOT_SHIM_CFLAGS) $(WARNINGS) \
		-Iinclude -std=c11 $< $(ANDROID_ROOT_SHIM_LDFLAGS) -ldl -o $@

build/test-android-root-shim: tests/android-root-shim.c \
		src/android_root_shim.c include/tgcompat/android_root.h | build
	$(GLIBC_CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 \
		tests/android-root-shim.c src/android_root_shim.c -ldl -o $@

build/test-exec-shim-driver: tests/exec-shim-driver.c | build
	$(GLIBC_CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $< -o $@

build/test-exec-shim-target: tests/exec-shim-target.c | build
	$(GLIBC_CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $< \
		-Wl,--dynamic-linker=/no/tgcompat-ld.so -o $@

build/test-sem-store: tests/sem-store.c build/sem_store.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

build/test-protocol: tests/protocol.c build/protocol.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

build/test-broker-dispatch: tests/broker-dispatch.c $(CORE_OBJECTS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

build/test-transport: tests/transport.c build/protocol.o build/transport.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 \
		$(THREAD_FLAGS) $^ $(LDFLAGS) -o $@

build/test-transport-security: tests/transport-security.c build/protocol.o \
		build/transport.o | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 $^ $(LDFLAGS) -o $@

build/test-broker-integration: tests/broker-integration.c build/protocol.o \
		build/transport.o build/tgcompatd | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 \
		tests/broker-integration.c build/protocol.o build/transport.o \
		$(LDFLAGS) -o $@

build/test-client: tests/client.c $(CLIENT_OBJECTS) build/tgcompatd | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 \
		tests/client.c $(CLIENT_OBJECTS) $(LDFLAGS) -o $@

build/benchmark-broker-roundtrip: benchmarks/broker-roundtrip.c \
		$(CLIENT_OBJECTS) build/tgcompatd | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Iinclude -std=c11 \
		benchmarks/broker-roundtrip.c $(CLIENT_OBJECTS) \
		$(LDFLAGS) -o $@

benchmark: build/benchmark-broker-roundtrip
	./build/benchmark-broker-roundtrip

exec-shim: build/libtgcompat-exec.so

check-exec-shim: build/libtgcompat-exec.so build/test-exec-shim-driver \
		build/test-exec-shim-target
	$(BASH) ./scripts/test-exec-shim.sh

release:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(RELEASE_CFLAGS) $(RELEASE_CPU_FLAGS)" \
		LDFLAGS="$(RELEASE_LDFLAGS)" build/tgcompatd \
		build/libtgcompat-client.a build/libtgcompat-exec.so \
		build/libtgcompat-android-root.so \
		build/benchmark-broker-roundtrip
	$(STRIP) --strip-unneeded build/tgcompatd \
		build/libtgcompat-exec.so build/libtgcompat-android-root.so \
		build/benchmark-broker-roundtrip

check: all check-exec-shim
	./scripts/run-probes.sh --no-build
	./build/test-sem-store
	./build/test-protocol
	./build/test-broker-dispatch
	./build/test-transport
	./build/test-transport-security
	./build/test-broker-integration
	./build/test-client
	./build/test-android-root-shim

check-broker: $(TESTS)
	./build/test-sem-store
	./build/test-protocol
	./build/test-broker-dispatch
	./build/test-transport
	./build/test-transport-security
	./build/test-broker-integration
	./build/test-client

clean:
	$(RM) -r build
