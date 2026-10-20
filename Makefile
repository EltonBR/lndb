CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=
LDLIBS ?=
BUILD_DIR ?= build
VERSION_FILE := VERSION
PREFIX ?= $(HOME)/.local
DESTDIR ?=
BINDIR ?= $(PREFIX)/bin
SBINDIR ?= $(PREFIX)/sbin

VERSION_HEADER := $(BUILD_DIR)/generated/lndb_version.h
override CPPFLAGS += -D_FILE_OFFSET_BITS=64 -D_POSIX_C_SOURCE=200809L -Iinclude -I$(BUILD_DIR)/generated

COMMON_SRC := $(wildcard common/*.c)
CLIENT_SRC := $(wildcard client/*.c)
DAEMON_SRC := $(wildcard daemon/*.c)
COMMON_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(COMMON_SRC))
CLIENT_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(CLIENT_SRC))
DAEMON_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(DAEMON_SRC))
CLIENT_BIN := $(BUILD_DIR)/lndb
DAEMON_BIN := $(BUILD_DIR)/lndbd

.PHONY: all client daemon test install clean version version-bump FORCE_VERSION_HEADER
all: version-bump client daemon

version:
	@printf 'LNDB %s\n' "$$(cat $(VERSION_FILE))"

client: version-bump $(CLIENT_BIN)
daemon: version-bump $(DAEMON_BIN)

test: $(BUILD_DIR)/tests/local_test $(BUILD_DIR)/tests/network_test $(DAEMON_BIN)
	./$(BUILD_DIR)/tests/local_test
	./$(BUILD_DIR)/tests/network_test $(DAEMON_BIN)

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(SBINDIR)
	install -m 0755 $(CLIENT_BIN) $(DESTDIR)$(BINDIR)/lndb
	install -m 0755 $(DAEMON_BIN) $(DESTDIR)$(SBINDIR)/lndbd

$(CLIENT_BIN): $(VERSION_HEADER) $(COMMON_OBJ) $(CLIENT_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

$(DAEMON_BIN): $(VERSION_HEADER) $(COMMON_OBJ) $(DAEMON_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

$(BUILD_DIR)/tests/local_test: tests/local_test.c $(COMMON_OBJ) $(BUILD_DIR)/client/transfer.o $(BUILD_DIR)/daemon/transfer.o
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/tests/network_test: tests/network_test.c $(BUILD_DIR)/common/protocol.o
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/client/main.o $(BUILD_DIR)/daemon/main.o: $(VERSION_HEADER)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

version-bump:
	@set -eu; \
	lock='$(VERSION_FILE).lock'; \
	until mkdir "$$lock" 2>/dev/null; do sleep 0.05; done; \
	trap 'rmdir "$$lock"' EXIT HUP INT TERM; \
	version=$$(cat '$(VERSION_FILE)'); \
	case "$$version" in *.*.*) ;; *) echo 'invalid VERSION (expected MAJOR.MINOR.PATCH)' >&2; exit 1;; esac; \
	major=$${version%%.*}; rest=$${version#*.}; minor=$${rest%%.*}; patch=$${rest#*.}; \
	case "$$major" in ''|*[!0-9]*) echo 'invalid VERSION (expected numeric MAJOR.MINOR.PATCH)' >&2; exit 1;; esac; \
	case "$$minor" in ''|*[!0-9]*) echo 'invalid VERSION (expected numeric MAJOR.MINOR.PATCH)' >&2; exit 1;; esac; \
	case "$$patch" in ''|*[!0-9]*) echo 'invalid VERSION (expected numeric MAJOR.MINOR.PATCH)' >&2; exit 1;; esac; \
	patch=$$((patch + 1)); \
	tmp='$(VERSION_FILE).tmp.'$$$$; \
	printf '%s.%s.%s\n' "$$major" "$$minor" "$$patch" > "$$tmp"; \
	mv "$$tmp" '$(VERSION_FILE)'

$(VERSION_HEADER): version-bump FORCE_VERSION_HEADER
	@mkdir -p $(dir $@)
	@{ \
		printf '%s\n' '#ifndef LNDB_VERSION_H' '#define LNDB_VERSION_H'; \
		printf '#define LNDB_VERSION "%s"\n' "$$(cat $(VERSION_FILE))"; \
		printf '%s\n' '#endif'; \
	} > $@.tmp
	@mv $@.tmp $@

clean:
	rm -rf $(BUILD_DIR)
