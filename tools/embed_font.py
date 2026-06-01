#!/usr/bin/env python3
"""
embed_font.py — convert a TTF file into a C header containing the bytes
as a static array. Used by z-console to bundle JetBrainsMono-Regular.ttf
directly into the executable so the resulting binary needs zero external
font files at runtime.

Output header defines:
    static const unsigned char  <var>[];
    static const unsigned int   <var>_len;

If the source file is missing, writes a zero-length stub instead so the
including code can fall through to file-based lookup without a build break.

Usage:
    python3 tools/embed_font.py SRC.ttf OUT.h VAR_NAME
"""
import os, sys

def main(argv):
    if len(argv) != 4:
        sys.stderr.write("usage: embed_font.py SRC.ttf OUT.h VAR_NAME\n")
        return 1
    src, out, var = argv[1], argv[2], argv[3]
    if os.path.exists(src):
        with open(src, "rb") as fh: data = fh.read()
    else:
        data = b""
    with open(out, "w") as f:
        f.write(f"/* Auto-generated from {os.path.basename(src) if data else '(none)'}.\n")
        f.write(f" * Do not edit; regenerate with tools/embed_font.py. */\n")
        f.write(f"static const unsigned char {var}[] = {{\n")
        if data:
            for i in range(0, len(data), 16):
                chunk = data[i:i+16]
                f.write("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
        else:
            f.write("  0\n")
        f.write("};\n")
        f.write(f"static const unsigned int {var}_len = {len(data)};\n")
    sys.stderr.write(f"wrote {out}: {len(data)} bytes embedded as `{var}`\n")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
