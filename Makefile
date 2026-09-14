# evergram-irc - a local IRC bridge for an Evergram account.
#
# Links against the sibling evergram-sdk-c checkout. Point EVERGRAM_DIR
# elsewhere if the SDK lives somewhere else.

# Absolute from here on: a relative path baked into an rpath is resolved against
# the *current directory* at runtime, so a binary that works from the project
# root breaks when run from build/ (and then silently picks up whatever
# libevergram.so happens to be installed system-wide).
EVERGRAM_DIR ?= ../evergram-sdk-c
EVERGRAM_DIR := $(abspath $(EVERGRAM_DIR))

CC      ?= cc
STD      = -std=c17
WARN     = -Wall -Wextra -Wpedantic
OPT     ?= -O2 -g
DEFS     = -D_POSIX_C_SOURCE=200809L
INCLUDES = -Iinclude -I$(EVERGRAM_DIR)/include
CFLAGS   = $(STD) $(WARN) -Werror $(OPT) $(DEFS) $(INCLUDES) -pthread
LDFLAGS  = -pthread

# The SDK is linked statically on purpose. This is an application, not a
# library: a static link has no runtime path to get wrong, and cannot be
# shadowed by an older libevergram.so installed on the machine. The transitive
# dependencies come from the SDK's own LIBS list.
SDK_STATIC = $(EVERGRAM_DIR)/build/lib/libevergram.a
LIBS     = $(SDK_STATIC) -lsodium -lwebsockets -lprotobuf-c -lssl -lcrypto

BUILD_DIR = build
OBJ_DIR   = $(BUILD_DIR)/obj
BIN       = $(BUILD_DIR)/evergram-irc

SOURCES = \
	src/main.c \
	src/irc.c \
	src/bridge.c

OBJECTS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(SOURCES))
HEADERS = $(wildcard include/*.h src/*.h) $(wildcard $(EVERGRAM_DIR)/include/*.h)

# The SDK is a prerequisite: its headers and library must exist first.
SDK_LIB = $(SDK_STATIC)

all: $(BIN)

$(OBJ_DIR)/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJECTS) $(SDK_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $@

$(SDK_LIB):
	@test -f $@ || { \
		echo "the SDK is not built: run make in $(EVERGRAM_DIR) first"; \
		exit 1; }

# Drives the bridge over TCP and checks the replies. No gateway needed: the
# bridge serves IRC while its Evergram connection is down.
test: $(BIN)
	@python3 tests/irc_smoke.py $(BIN)

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "evergram-irc targets:"
	@echo "  all     build the bridge (needs the SDK built in $(EVERGRAM_DIR))"
	@echo "  test    run the IRC protocol smoke test"
	@echo "  clean   remove build/"

.PHONY: all test clean help
