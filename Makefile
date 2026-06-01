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

# Enable with `make SQLITE=1`. Links against libsqlite3.
#   apt-get install libsqlite3-dev   /   brew install sqlite
SQLITE ?= 0
ifeq ($(SQLITE),1)
    CFLAGS += -DZ_WITH_SQLITE
    LDLIBS += -lsqlite3
endif

# Enable with `make CV=1`. Embedded face detection — no extra libs, no
# Python. Read a Haar cascade as a packed binary produced by
# `tools/cascade_to_bin.py`. Inputs are PGM (use (img:grayscale) from
# IMAGE=1 to get there from JPG/PNG).
CV ?= 0
ifeq ($(CV),1)
    CFLAGS += -DZ_WITH_CV
endif

# Enable with `make OCR=1`. Links against libtesseract + libleptonica
# (the tesseract C API). Replaces the python+pytesseract path.
#   apt-get install libtesseract-dev libleptonica-dev tesseract-ocr
#   brew install tesseract
#
# Header/lib discovery, in order of preference:
#   1. pkg-config tesseract     (works on Linux + modern macOS Homebrew)
#   2. brew --prefix tesseract  (macOS fallback if pkg-config isn't on PATH)
#   3. plain -ltesseract -lleptonica (assumes default search path)
OCR ?= 0
ifeq ($(OCR),1)
    # Start with whatever pkg-config knows for tesseract...
    TESS_CFLAGS := $(shell pkg-config --cflags tesseract 2>/dev/null)
    TESS_LIBS   := $(shell pkg-config --libs   tesseract 2>/dev/null)
    # ...and union with leptonica's own .pc (sometimes called `lept`),
    # because tesseract.pc doesn't always Require leptonica.
    LEPT_CFLAGS := $(shell pkg-config --cflags lept 2>/dev/null)
    ifeq ($(LEPT_CFLAGS),)
        LEPT_CFLAGS := $(shell pkg-config --cflags leptonica 2>/dev/null)
    endif
    LEPT_LIBS := $(shell pkg-config --libs lept 2>/dev/null)
    ifeq ($(LEPT_LIBS),)
        LEPT_LIBS := $(shell pkg-config --libs leptonica 2>/dev/null)
    endif
    TESS_CFLAGS += $(LEPT_CFLAGS)
    TESS_LIBS   += $(LEPT_LIBS)
    # If pkg-config gave us nothing at all, try Homebrew prefixes.
    ifeq ($(strip $(TESS_CFLAGS) $(TESS_LIBS)),)
        TESS_PREFIX := $(shell brew --prefix tesseract 2>/dev/null)
        ifneq ($(TESS_PREFIX),)
            LEPT_PREFIX := $(shell brew --prefix leptonica 2>/dev/null)
            TESS_CFLAGS := -I$(TESS_PREFIX)/include
            TESS_LIBS   := -L$(TESS_PREFIX)/lib -ltesseract
            ifneq ($(LEPT_PREFIX),)
                TESS_CFLAGS += -I$(LEPT_PREFIX)/include
                TESS_LIBS   += -L$(LEPT_PREFIX)/lib -lleptonica
            else
                TESS_LIBS   += -lleptonica
            endif
        else
            TESS_LIBS := -ltesseract -lleptonica
        endif
    else ifeq ($(LEPT_CFLAGS),)
        # pkg-config had tesseract but NOT leptonica. Add brew --prefix for
        # leptonica as a belt-and-suspenders measure — that's the common
        # Homebrew failure mode that triggered this fix.
        LEPT_PREFIX := $(shell brew --prefix leptonica 2>/dev/null)
        ifneq ($(LEPT_PREFIX),)
            TESS_CFLAGS += -I$(LEPT_PREFIX)/include
            TESS_LIBS   += -L$(LEPT_PREFIX)/lib -lleptonica
        else
            TESS_LIBS   += -lleptonica
        endif
    endif
    CFLAGS += -DZ_WITH_OCR $(TESS_CFLAGS)
    LDLIBS += $(TESS_LIBS)
endif

# Enable with `make LIBCURL=1`. Links libcurl directly instead of shelling
# out to the `curl` binary. Same http:get / http:post signatures; the opts
# object adds verify-ssl / follow-redirects / timeout / user-agent keys.
#   apt-get install libcurl4-openssl-dev
#   brew install curl   # pkg-config sees libcurl
#
# Discovery: pkg-config libcurl  →  brew --prefix curl  →  bare -lcurl
LIBCURL ?= 0
ifeq ($(LIBCURL),1)
    CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
    CURL_LIBS   := $(shell pkg-config --libs   libcurl 2>/dev/null)
    ifeq ($(strip $(CURL_CFLAGS) $(CURL_LIBS)),)
        CURL_PREFIX := $(shell brew --prefix curl 2>/dev/null)
        ifneq ($(CURL_PREFIX),)
            CURL_CFLAGS := -I$(CURL_PREFIX)/include
            CURL_LIBS   := -L$(CURL_PREFIX)/lib -lcurl
        else
            CURL_LIBS   := -lcurl
        endif
    endif
    CFLAGS += -DZ_WITH_LIBCURL $(CURL_CFLAGS)
    LDLIBS += $(CURL_LIBS)
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
	@echo "SQLITE:   $(SQLITE)"
	@echo "OCR:      $(OCR)"
	@echo "CV:       $(CV)"
	@echo "LIBCURL:  $(LIBCURL)"

$(BIN): $(SRC) z_img.h z_vision.h z_sqlite.h z_ocr.h z_cv.h z_http.h
ifeq ($(PLATFORM),windows)
	@if not exist "$(DIST_DIR)" mkdir "$(subst /,\,$(DIST_DIR))"
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS) $(LDLIBS)
	@copy /Y "$(subst /,\,$(BIN))" z$(EXT) >nul
else
	@mkdir -p $(DIST_DIR)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS) $(LDLIBS)
	@ln -sf $(BIN) z
endif

$(IDE_BIN): zide.c $(SRC) z_img.h z_vision.h z_sqlite.h z_ocr.h z_cv.h z_http.h
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
