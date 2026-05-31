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
ifeq ($(UNAME_S),Darwin)
    LDFLAGS += -framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL
endif
ifeq ($(UNAME_S),Linux)
    LDFLAGS += -lm -lpthread -ldl -lrt -lX11
endif

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC) ../z.c ../z_img.h ../z_vision.h ../z_sqlite.h ../z_ocr.h ../z_cv.h ../z_html.h ../version.h
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)
