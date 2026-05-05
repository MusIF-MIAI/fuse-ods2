# fuse-ods2 - read-only FUSE driver for ODS-2 (Files-11)

UNAME_S := $(shell uname -s)

CC      ?= cc
AR      ?= ar
RANLIB  ?= ranlib

CSTD    := -std=c99
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
           -Wno-sign-compare -Wno-missing-field-initializers
DEFS    := -D_FILE_OFFSET_BITS=64 -D_POSIX_C_SOURCE=200809L
OPT     ?= -O2 -g

CFLAGS  ?= $(CSTD) $(WARN) $(OPT)
CFLAGS  += $(DEFS) -Iods2lib -Isrc

# Pick the right FUSE flavour for the host.
ifeq ($(UNAME_S),Linux)
    FUSE_PKG := fuse3
endif
ifeq ($(UNAME_S),Darwin)
    # macFUSE ships a libfuse-compatible API.  Prefer fuse3 if available,
    # fall back to fuse (libfuse2-style) otherwise.
    FUSE_PKG := $(shell pkg-config --exists fuse3 && echo fuse3 || echo fuse)
    DEFS    += -D_DARWIN_C_SOURCE
endif

FUSE_PKG ?= fuse3

FUSE_CFLAGS := $(shell pkg-config --cflags $(FUSE_PKG) 2>/dev/null)
FUSE_LIBS   := $(shell pkg-config --libs   $(FUSE_PKG) 2>/dev/null)

BUILD := build

LIB_SRCS := \
    ods2lib/access.c   \
    ods2lib/cache.c    \
    ods2lib/compat.c   \
    ods2lib/device.c   \
    ods2lib/direct.c   \
    ods2lib/vmstime.c

LIB_OBJS := $(LIB_SRCS:%.c=$(BUILD)/%.o)

# Wrapper sources split by whether they pull in <fuse.h>.  The standalone
# pieces always compile, so 'make objs' is a useful sanity check on hosts
# without libfuse3 installed.
NO_FUSE_SRCS := \
    src/compat_glue.c \
    src/phyfuse.c

FUSE_SRCS := \
    src/fuse_ods2.c \
    src/lookup.c    \
    src/ops.c

NO_FUSE_OBJS := $(NO_FUSE_SRCS:%.c=$(BUILD)/%.o)
FUSE_OBJS    := $(FUSE_SRCS:%.c=$(BUILD)/%.o)
APP_OBJS     := $(NO_FUSE_OBJS) $(FUSE_OBJS)

LIBODS2  := $(BUILD)/libods2.a
BIN      := fuse-ods2

.PHONY: all clean lib objs bin
all: bin

lib: $(LIBODS2)

# objs builds only the FUSE-independent wrapper code so it is useful
# even on hosts without libfuse3 installed (e.g. macOS without macFUSE).
objs: $(NO_FUSE_OBJS)

bin: $(BIN)

$(BIN): $(APP_OBJS) $(LIBODS2)
ifeq ($(strip $(FUSE_LIBS)),)
	@echo "*** $(FUSE_PKG) not found via pkg-config; install libfuse3-dev"
	@echo "    (or macFUSE on macOS) and re-run make."
	@false
else
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $(APP_OBJS) $(LIBODS2) $(FUSE_LIBS)
endif

$(LIBODS2): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(LIB_OBJS)
	$(RANLIB) $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD) fuse-ods2

-include $(LIB_OBJS:.o=.d) $(APP_OBJS:.o=.d)
