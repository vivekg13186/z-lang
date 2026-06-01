#!/usr/bin/env python3
"""
extract_menlo.py — pull a single-face Menlo-Regular.ttf out of macOS's
system Menlo.ttc (a TrueType Collection) so raylib's stb_truetype loader
can actually parse it.

Why this exists: many Menlo-Regular.ttf copies floating around the
internet are FontForge-edited Mac-only fonts whose cmap table has only
Macintosh-platform encodings. raylib's stb_truetype only knows how to
read Microsoft/Unicode cmaps, so it bails out. Extracting fresh from
the macOS Menlo.ttc gives a font with the right cmap tables.

Usage:
    pip install fonttools
    python3 tools/extract_menlo.py [out-path]

`out-path` defaults to ./Menlo-Regular.ttf (next to wherever you run it).
Source defaults to /System/Library/Fonts/Menlo.ttc.

For non-macOS systems: substitute any monospace TTF — JetBrains Mono and
Fira Code both ship with proper Unicode cmaps that raylib accepts.
"""
import os, sys

DEFAULT_SRC = "/System/Library/Fonts/Menlo.ttc"

def main(argv):
    out = argv[1] if len(argv) >= 2 else "Menlo-Regular.ttf"
    src = argv[2] if len(argv) >= 3 else DEFAULT_SRC
    if not os.path.exists(src):
        sys.stderr.write(
            f"source not found: {src}\n"
            "on non-macOS systems pass an explicit second arg, or use any\n"
            "other monospace TTF (JetBrains Mono / Fira Code / DejaVu Sans Mono).\n")
        return 1
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        sys.stderr.write("fonttools not installed — run: pip install fonttools\n")
        return 1
    font = TTFont(src, fontNumber=0)   # face 0 is Regular in Menlo.ttc
    font.save(out)
    sys.stderr.write(f"wrote {out} ({os.path.getsize(out)} bytes)\n")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
