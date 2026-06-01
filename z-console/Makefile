# z-console — raylib GUI REPL for z
#
# Needs raylib installed (and pkg-config to find it):
#   macOS:  brew install raylib pkg-config
#   Linux:  sudo apt-get install libraylib-dev pkg-config
#
# Then:  make  &&  ./z-console

TARGET := z-console
SRC    := z-console.c

CFLAGS  := -O2 -std=c99 -Wall -Wextra \
           -Wno-unused-parameter -Wno-unused-result -Wno-unused-function \
           -Wno-stringop-truncation -Wno-clobbered \
           -I..
LDFLAGS :=

# --- Optional modules ---------------------------------------------------
# Pass IMAGE=1 to include img:* builtins (needs ImageMagick at runtime).
# Pass VISION=1 to include vision:* builtins (needs zbarimg / opencv-python / alpr).
# Pass SQLITE=1 to include sqlite:* / kv:* builtins (links libsqlite3).
IMAGE  ?= 0
VISION ?= 0
SQLITE ?= 0
ifeq ($(IMAGE),1)
    CFLAGS += -DZ_WITH_IMAGE
endif
ifeq ($(VISION),1)
    CFLAGS += -DZ_WITH_VISION
endif
ifeq ($(SQLITE),1)
    CFLAGS  += -DZ_WITH_SQLITE
    LDFLAGS += -lsqlite3
endif
CV ?= 0
ifeq ($(CV),1)
    CFLAGS  += -DZ_WITH_CV
endif
OCR ?= 0
ifeq ($(OCR),1)
    TESS_CFLAGS := $(shell pkg-config --cflags tesseract 2>/dev/null)
    TESS_LIBS   := $(shell pkg-config --libs   tesseract 2>/dev/null)
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
        LEPT_PREFIX := $(shell brew --prefix leptonica 2>/dev/null)
        ifneq ($(LEPT_PREFIX),)
            TESS_CFLAGS += -I$(LEPT_PREFIX)/include
            TESS_LIBS   += -L$(LEPT_PREFIX)/lib -lleptonica
        else
            TESS_LIBS   += -lleptonica
        endif
    endif
    CFLAGS  += -DZ_WITH_OCR $(TESS_CFLAGS)
    LDFLAGS += $(TESS_LIBS)
endif

# raylib via pkg-config
RAYLIB_CFLAGS := $(shell pkg-config --cflags raylib 2>/dev/null)
RAYLIB_LIBS   := $(shell pkg-config --libs   raylib 2>/dev/null)
CFLAGS  += $(RAYLIB_CFLAGS)
LDFLAGS += $(RAYLIB_LIBS)

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_S),Darwin)
    PLATFORM := macos
    EXT      :=
    LDFLAGS += -framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL
endif
ifeq ($(UNAME_S),Linux)
    PLATFORM := linux
    EXT      :=
    LDFLAGS += -lm -lpthread -ldl -lrt -lX11
endif
ifneq (,$(findstring MINGW,$(UNAME_S)))
    PLATFORM := windows
    EXT      := .exe
endif
ifneq (,$(findstring MSYS,$(UNAME_S)))
    PLATFORM := windows
    EXT      := .exe
endif

# Normalise architecture tag to match the top-level Makefile's layout
# (dist/macos_arm64, dist/linux_x86, dist/macos_x86, ...).
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

DIST_DIR := ../dist/$(PLATFORM)_$(ARCH)
DIST_BIN := $(DIST_DIR)/$(TARGET)$(EXT)

.PHONY: all clean run info

all: $(TARGET) $(DIST_BIN)

info:
	@echo "platform: $(PLATFORM)"
	@echo "arch:     $(ARCH)"
	@echo "binary:   $(TARGET)$(EXT)"
	@echo "dist:     $(DIST_BIN)"
	@echo "CFLAGS:   $(CFLAGS)"
	@echo "IMAGE:    $(IMAGE)"
	@echo "VISION:   $(VISION)"
	@echo "SQLITE:   $(SQLITE)"
	@echo "OCR:      $(OCR)"
	@echo "CV:       $(CV)"

# Bundle JetBrainsMono-Regular.ttf into the binary by embedding its bytes
# as a C array. Re-runs whenever the .ttf changes; produces a zero-length
# stub when the .ttf is absent so the build still succeeds (and the loader
# falls through to file-based lookup).
FONT_TTF := JetBrainsMono-Regular.ttf
FONT_H   := zc_font_embed.h

$(FONT_H): $(wildcard $(FONT_TTF))
	@python3 ../tools/embed_font.py $(FONT_TTF) $(FONT_H) zc_jbm_font

$(TARGET): $(SRC) $(FONT_H) ../z.c ../z_img.h ../z_vision.h ../z_sqlite.h ../z_ocr.h ../z_cv.h ../z_html.h ../version.h
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)$(EXT) $(LDFLAGS)

# Stage the freshly built binary into ../dist/<platform>_<arch>/ so the
# layout matches the top-level Makefile's output for `z` and `zide`.
$(DIST_BIN): $(TARGET)
	@mkdir -p $(DIST_DIR)
	@cp $(TARGET)$(EXT) $(DIST_BIN)
	@echo "→ $(DIST_BIN)"

clean:
	rm -f $(TARGET)$(EXT) $(DIST_BIN) $(FONT_H)

run: $(TARGET)
	./$(TARGET)$(EXT)
