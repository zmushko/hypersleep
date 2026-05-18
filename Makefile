# Renatum Makefile
# Builds renatumd (daemon) and renatum (CLI) as separate binaries
# that share a common library of internal modules.

PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
SBINDIR     ?= $(PREFIX)/sbin
SYSCONFDIR  ?= /etc
LOCALSTATEDIR ?= /var
SYSTEMDDIR  ?= /etc/systemd/system

CC          ?= gcc
CFLAGS      ?= -O2 -g -Wall -Wextra -Wpedantic -Wno-unused-parameter
CFLAGS      += -std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
CPPFLAGS    += -Iinclude -Ithird_party/librnotify

LDFLAGS     ?=
LDLIBS      += -lcrypto -llmdb -lpthread

# Optional: zstd compression
WITH_ZSTD   ?= 0
ifeq ($(WITH_ZSTD),1)
    CPPFLAGS += -DWITH_ZSTD=1
    LDLIBS   += -lzstd
endif

# librnotify (git submodule, static link)
LIBRNOTIFY_DIR := third_party/librnotify
LIBRNOTIFY_A   := $(LIBRNOTIFY_DIR)/librnotify.a

# Shared objects between daemon and CLI
COMMON_SRCS := \
    src/config.c \
    src/log.c \
    src/store.c \
    src/index.c \
    src/timeparse.c

# Daemon-only
DAEMON_SRCS := \
    src/main.c \
    src/watcher.c \
    src/debounce.c \
    src/snapshot.c \
    src/control.c

# CLI-only
CLI_SRCS := \
    src/cli.c \
    src/cmd_status.c \
    src/cmd_log.c \
    src/cmd_show.c \
    src/cmd_diff.c \
    src/cmd_restore.c \
    src/cmd_find.c \
    src/cmd_prune.c \
    src/cmd_verify.c \
    src/cmd_config.c \
    src/restore.c \
    src/retention.c

COMMON_OBJS := $(COMMON_SRCS:.c=.o)
DAEMON_OBJS := $(DAEMON_SRCS:.c=.o) $(COMMON_OBJS)
CLI_OBJS    := $(CLI_SRCS:.c=.o) $(COMMON_OBJS)

all: renatumd renatum

renatumd: $(DAEMON_OBJS) $(LIBRNOTIFY_A)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

renatum: $(CLI_OBJS) $(LIBRNOTIFY_A)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(LIBRNOTIFY_A):
	@if [ ! -f $(LIBRNOTIFY_DIR)/Makefile ]; then \
		echo "librnotify submodule not initialized."; \
		echo "Run: git submodule update --init --recursive"; \
		exit 1; \
	fi
	$(MAKE) -C $(LIBRNOTIFY_DIR) static

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Installation

install: renatumd renatum
	install -d $(DESTDIR)$(SBINDIR) $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(SYSCONFDIR)/renatum
	install -d $(DESTDIR)$(SYSTEMDDIR)
	install -d $(DESTDIR)$(LOCALSTATEDIR)/lib/renatum/store
	install -d $(DESTDIR)$(LOCALSTATEDIR)/lib/renatum/index
	install -d $(DESTDIR)$(LOCALSTATEDIR)/log/renatum
	install -m 0755 renatumd $(DESTDIR)$(SBINDIR)/renatumd
	install -m 0755 renatum  $(DESTDIR)$(BINDIR)/renatum
	install -m 0644 etc/renatum.conf.example \
	    $(DESTDIR)$(SYSCONFDIR)/renatum/renatum.conf.example
	install -m 0644 systemd/renatumd.service \
	    $(DESTDIR)$(SYSTEMDDIR)/renatumd.service

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/renatumd
	rm -f $(DESTDIR)$(BINDIR)/renatum
	rm -f $(DESTDIR)$(SYSTEMDDIR)/renatumd.service
	@echo "Configuration in $(SYSCONFDIR)/renatum/ left intact."
	@echo "Data in $(LOCALSTATEDIR)/lib/renatum/ left intact."

# Development

.PHONY: clean test check format

clean:
	rm -f renatumd renatum
	rm -f src/*.o
	$(MAKE) -C $(LIBRNOTIFY_DIR) clean 2>/dev/null || true

test: renatumd renatum
	@echo "Unit tests not yet implemented."
	@echo "End-to-end stress tests live in tests/stress/"
	@for f in tests/stress/*.sh; do \
	    echo "Running $$f"; \
	    bash $$f || exit 1; \
	done

check:
	@command -v cppcheck >/dev/null || { echo "cppcheck not installed"; exit 1; }
	cppcheck --enable=all --suppress=missingIncludeSystem \
	    --error-exitcode=1 -Iinclude src/

format:
	@command -v clang-format >/dev/null || { echo "clang-format not installed"; exit 1; }
	clang-format -i src/*.c include/*.h

.PHONY: all install uninstall
