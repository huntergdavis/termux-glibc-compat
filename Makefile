CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=

WARNINGS := -Wall -Wextra -Werror -Wpedantic
PROBE_NAMES := pthread-basic robust-list sysv-semaphore sysv-shm
PROBES := $(addprefix build/,$(PROBE_NAMES))

.PHONY: all check clean

all: $(PROBES)

build:
	mkdir -p $@

build/%: probes/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -pthread $< $(LDFLAGS) -o $@

check: all
	./scripts/run-probes.sh --no-build

clean:
	$(RM) -r build
