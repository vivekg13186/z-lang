# Cross-platform Makefile for the z interpreter.
# Works with GNU make on macOS, Linux, and Windows (via MinGW / MSYS2).
#
# Output:
#   dist/<os>_<arch>/z[.exe]
#
# e.g. on a Linux x86_64 box you'll get:
#   dist/linux_x86/z

CC      ?= cc
SRC     := z.c
PREFIX  ?= /usr/local

# --- Detect host OS and architecture -------------------------------------
ifeq ($(OS),Windows_NT)
    PLATFORM := windows
    EXT      := .exe
    LDLIBS   :=
    # ARM Windows is uncommon; default to x86 unless overridden.
    ARCH     := x86
    # Force cmd.exe so recipes like `if not exist ... mkdir ...` are parsed by
    # cmd, not by sh from MSYS/Git Bash (which would otherwise report
    # "syntax error: unexpected end of file" on the `if`).
    SHELL       := cmd.exe
    .SHELLFLAGS := /C
else
    UNAME_S := $(shell uname -s)
    UNAME_M := $(shell uname -m)
    ifeq ($(UNAME_S),Darwin)
        PLATFORM := macos
    else
        PLATFORM := linux
    endif
    EXT      :=
    LDLIBS   := -lm
    # Normalise machine name into a short arch tag.
    ifeq ($(UNAME_M),x86_64)
        ARCH := x86
    else ifeq ($(UNAME_M),amd64)
        ARCH := x86
    else ifeq ($(UNAME_M),aarch64)
        ARCH := arm64
    else ifeq ($(UNAME_M),arm64)
        ARCH := arm64
    else ifeq ($(UNAME_M),i686)
        ARCH := x86_32
    else
        ARCH := $(UNAME_M)
    endif
endif

DIST_DIR := dist/$(PLATFORM)_$(ARCH)
BIN      := $(DIST_DIR)/z$(EXT)
IDE_BIN  := $(DIST_DIR)/zide$(EXT)

CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -Wno-unused-function
LDFLAGS ?=

# --- Optional modules ----------------------------------------------------
# Enable with `make IMAGE=1`. Shells out to ImageMagick at runtime.
IMAGE ?= 0
ifeq ($(IMAGE),1)
    CFLAGS += -DZ_WITH_IMAGE
endif

# Enable with `make VISION=1`. Shells out to zbarimg / opencv-python / alpr.
VISION ?= 0
ifeq ($(VISION),1)
    CFLAGS += -DZ_WITH_VISION
endif

.PHONY: all clean install test run info zide release deb

# Override on the command line:  make release VERSION=0.2.0
VERSION ?= 0.1.0

all: $(BIN) $(IDE_BIN)

# Build just the enhanced REPL.
zide: $(IDE_BIN)

info:
	@echo "platform: $(PLATFORM)"
	@echo "arch:     $(ARCH)"
	@echo "binary:   $(BIN)"
	@echo "CC:       $(CC)"
	@echo "CFLAGS:   $(CFLAGS)"
	@echo "IMAGE:    $(IMAGE)"
	@echo "VISION:   $(VISION)"

$(BIN): $(SRC) z_img.h z_vision.h
ifeq ($(PLATFORM),windows)
	@if not exist "$(DIST_DIR)" mkdir "$(subst /,\,$(DIST_DIR))"
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS) $(LDLIBS)
	@copy /Y "$(subst /,\,$(BIN))" z$(EXT) >nul
else
	@mkdir -p $(DIST_DIR)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS) $(LDLIBS)
	@ln -sf $(BIN) z
endif

$(IDE_BIN): zide.c $(SRC) z_img.h z_vision.h
ifeq ($(PLATFORM),windows)
	@if not exist "$(DIST_DIR)" mkdir "$(subst /,\,$(DIST_DIR))"
	$(CC) $(CFLAGS) zide.c -o $(IDE_BIN) $(LDFLAGS) $(LDLIBS)
	@copy /Y "$(subst /,\,$(IDE_BIN))" zide$(EXT) >nul
else
	@mkdir -p $(DIST_DIR)
	$(CC) $(CFLAGS) zide.c -o $(IDE_BIN) $(LDFLAGS) $(LDLIBS)
	@ln -sf $(IDE_BIN) zide
endif

clean:
ifeq ($(PLATFORM),windows)
	-if exist dist rmdir /S /Q dist
	-if exist build rmdir /S /Q build
	-if exist z.exe del z.exe
	-if exist zide.exe del zide.exe
	-del /Q z-lang_*.deb 2>nul
else
	rm -rf dist build
	rm -f z zide z-lang_*.deb
endif

install: $(BIN)
ifeq ($(PLATFORM),windows)
	@echo "On Windows, copy $(BIN) onto your PATH manually."
else
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/z
endif

test: $(BIN)
	$(BIN) examples/hello.z
	$(BIN) examples/adults.z
	$(BIN) examples/fib.z
	$(BIN) examples/json_demo.z
	$(BIN) examples/test_suite.z

run: $(BIN)
	$(BIN)

# Stage z + zide + examples into a tarball/zip with a SHA-256, for uploading
# as a GitHub Release asset. Run on each target platform you want to ship.
release: $(BIN) $(IDE_BIN)
	sh packaging/release.sh $(VERSION)

# Build a Debian .deb from the current Linux dist/ binaries.
deb: $(BIN) $(IDE_BIN)
ifeq ($(PLATFORM),linux)
	sh packaging/debian/build-deb.sh $(VERSION)
else
	@echo "deb: only builds on Linux (current platform: $(PLATFORM))"
	@exit 1
endif
