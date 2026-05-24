# Cross-platform Makefile for the z interpreter.
# Works with GNU make on macOS, Linux, and Windows (via MinGW / MSYS2).

CC      ?= cc
SRC     := z.c
PREFIX  ?= /usr/local

# Detect host OS so we set the right flags, libs, and binary name.
ifeq ($(OS),Windows_NT)
    PLATFORM := windows
    BIN      := z.exe
    LDLIBS   :=
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        PLATFORM := macos
        BIN      := z
        LDLIBS   := -lm
    else
        PLATFORM := linux
        BIN      := z
        LDLIBS   := -lm
    endif
endif

CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-result
LDFLAGS ?=

.PHONY: all clean install test run info

all: $(BIN)

info:
	@echo "platform: $(PLATFORM)"
	@echo "binary:   $(BIN)"
	@echo "CC:       $(CC)"
	@echo "CFLAGS:   $(CFLAGS)"

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS) $(LDLIBS)

clean:
ifeq ($(PLATFORM),windows)
	-del /Q $(BIN) 2>nul
else
	rm -f $(BIN)
endif

install: $(BIN)
ifeq ($(PLATFORM),windows)
	@echo "On Windows, copy $(BIN) onto your PATH manually."
else
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/z
endif

test: $(BIN)
	./$(BIN) examples/hello.z
	./$(BIN) examples/adults.z
	./$(BIN) examples/fib.z
	./$(BIN) examples/json_demo.z
	./$(BIN) examples/test_suite.z

run: $(BIN)
	./$(BIN)
