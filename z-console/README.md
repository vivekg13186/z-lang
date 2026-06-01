# z-console

A native GUI REPL for the z language, built with [raylib](https://www.raylib.com/).

Same evaluator as `z` and `zide`, but the output area can render **shapes, text,
and (with `IMAGE=1`) images** alongside the regular text output. Each `Enter`
creates a "cell": the echoed input, any `print` output, and a canvas with
whatever shapes you drew.

```
┌─ z-console ──────────────────────────────────────┐
│  z> (print "hello")                              │
│  hello                                           │
│                                                  │
│  z> (do                                          │
│       (ui:circle 100 100 50 "tomato")            │
│       (ui:rect   220 60 80 80 "steelblue")       │
│       (ui:text-at 20 200 "two shapes" 22 "black"))│
│  ┌─────────────────────────────────────────────┐ │
│  │       ●                ■                    │ │
│  │                                             │ │
│  │  two shapes                                 │ │
│  └─────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────┤
│  z> _                                            │
└──────────────────────────────────────────────────┘
```

## Build

This folder must sit next to `z.c` and `z_img.h` (the Makefile uses
`#include "../z.c"`).

```
z_lang/
├── z.c
├── z_img.h
└── z-console/
    ├── z-console.c
    ├── Makefile
    └── README.md
```

### macOS

```
brew install raylib pkg-config
cd z-console
make
./z-console
```

### Linux (Debian / Ubuntu)

```
sudo apt-get install libraylib-dev pkg-config
cd z-console
make
./z-console
```

### Windows

Install raylib via vcpkg or grab a pre-built dev-package, make sure
`pkg-config` resolves it, then build with MSYS2 / MinGW `make`.

### Optional modules

z-console reuses the same build flags as `z` itself. Without them, the
matching builtins aren't compiled in — which means **autocomplete won't
suggest them either** because there's nothing in the env to match. To get
`img:*` / `vision:*` / `sqlite:*` / `kv:*` in the autocomplete popup,
build with:

```sh
make IMAGE=1                       # img:* builtins (needs ImageMagick at runtime)
make VISION=1                      # vision:* builtins (needs zbarimg / opencv-python / alpr)
make SQLITE=1                      # sqlite:* + kv:* (links libsqlite3)
make IMAGE=1 VISION=1 SQLITE=1     # everything
```

Install the SQLite dev headers if you turn that flag on:

| OS              | How |
| --------------- | --- |
| macOS           | `brew install sqlite` |
| Debian / Ubuntu | `sudo apt-get install libsqlite3-dev` |
| Fedora          | `sudo dnf install sqlite-devel` |
| Arch            | `sudo pacman -S sqlite` |

Verify with `(help)` inside z-console — `image`, `vision`, and `sqlite`
topics only appear when their modules are compiled in.

## Fonts

z-console renders all text in **JetBrains Mono**, which is **embedded
directly into the binary at build time** — a freshly-built `z-console`
needs zero external font files. The embed step happens automatically
when the Makefile sees `z-console/JetBrainsMono-Regular.ttf` (already
present in this repo); `tools/embed_font.py` generates a C header
(`zc_font_embed.h`) of the TTF bytes, which `z-console.c` then hands to
raylib via `LoadFontFromMemory` on startup. Adds ~274 KB to the binary.

JetBrains Mono is OFL-licensed (freely redistributable), designed
specifically for code, and has a proper Microsoft/Unicode `cmap` table
that raylib's `stb_truetype` parses reliably.

**Install (recommended):**

| OS              | How |
| --------------- | --- |
| macOS           | `brew install --cask font-jetbrains-mono` |
| Debian / Ubuntu | `sudo apt-get install fonts-jetbrains-mono` |
| Arch            | `sudo pacman -S ttf-jetbrains-mono` |
| Fedora          | `sudo dnf install jetbrains-mono-fonts` |
| Any OS          | download from [jetbrains.com/lp/mono](https://www.jetbrains.com/lp/mono/) and drop `JetBrainsMono-Regular.ttf` into `z-console/` |

Resolution order at startup:

1. `$Z_CONSOLE_FONT` — explicit override; must point to a TTF.
2. **Embedded** — the JetBrains Mono bytes baked into the binary at
   build time (no file lookup, no install).
3. **Bundled file** — `JetBrainsMono-Regular.ttf` (or `JetBrainsMonoNL-Regular.ttf`,
   then legacy `Menlo-Regular.ttf` / `Menlo.ttf`) in the current working
   directory, or next to the `z-console` binary. Useful for swapping
   the font without rebuilding.
4. **System locations** — JetBrains Mono first, then Menlo:
   - macOS: `/Library/Fonts/JetBrainsMono-Regular.ttf`,
     `~/Library/Fonts/...`, `/System/Library/Fonts/Menlo.ttc`
   - Linux: `/usr/share/fonts/truetype/jetbrains-mono/...`,
     `~/.local/share/fonts/...`, `~/.fonts/...`
   - Windows: `C:\Windows\Fonts\JetBrainsMono-Regular.ttf`
4. Raylib's built-in pixel font (with a clear stderr message
   listing the install commands above).

**Use a different monospace** — `Fira Code`, `Cascadia Code`,
`DejaVu Sans Mono`, etc. all have proper Unicode cmaps:

```sh
Z_CONSOLE_FONT=~/Library/Fonts/FiraCode-Regular.ttf ./z-console
```

**Troubleshooting** — if you see two `FILEIO: ... loaded successfully`
lines back-to-back, your bundled TTF's `cmap` table has only Macintosh
platform encodings — `stb_truetype` rejects those silently. This bites
old extracted Menlo copies in particular. Fix on macOS:

```sh
pip install fonttools
python3 tools/extract_menlo.py z-console/Menlo-Regular.ttf
```

…but the simpler answer is just to install JetBrains Mono via the table
above.

```sh
Z_CONSOLE_FONT=~/Library/Fonts/JetBrainsMono-Regular.ttf ./z-console
```

## Builtins added on top of the standard z stdlib

All operate on the *current cell* — they're recorded during evaluation and
rendered in the cell's canvas:

| Function | What it does |
| -------- | ------------ |
| `(ui:text "line")` | append a line of text to the cell |
| `(ui:circle cx cy r [color])` | filled circle |
| `(ui:rect   x  y  w  h  [color])` | filled rectangle |
| `(ui:line   x1 y1 x2 y2 [color])` | straight line |
| `(ui:text-at x y "msg" [size] [color])` | text at an absolute position |
| `(ui:image "path" [x y] [w h])` | display an image inline in the cell |
| `(ui:polygon (array x1 y1 x2 y2 ...) [color])` | closed polygon outline |
| `(ui:triangle x1 y1 x2 y2 x3 y3 [color])` | triangle outline |
| `(ui:save-canvas "path")` | snapshot the whole window to a PNG |
| `(ui:clear)` | wipe the current cell's drawings |

`print` is intercepted: text written via `(print …)` appears in the cell
instead of (well, in addition to) stdout. Existing z programs work
unchanged.

### Colours

Pass a string. Named — `red`, `green`, `blue`, `yellow`, `orange`, `purple`,
`pink`, `white`, `black`, `gray`/`grey`, `darkgray`, `lightgray`,
`magenta`, `maroon`, `violet` — or hex: `"#ff8800"`, `"#ff880080"` (alpha).

## Keyboard

| Key                  | Action                                |
| -------------------- | ------------------------------------- |
| `Enter`              | run when parens balance; otherwise inserts a newline (so multi-line `(do ...)` and `(fn ...)` Just Work). Inside an open autocomplete popup it inserts the highlighted item. |
| `Shift+Enter`        | always insert a newline (force continuation) |
| `Tab`                | accept the highlighted autocomplete item |
| `↑` / `↓`            | navigate autocomplete when open; otherwise history. `↑` on an empty buffer reloads the most recent cell (edit-and-rerun). |
| `Esc`                | dismiss the autocomplete popup        |
| `←` / `→`            | move cursor by one character          |
| `Ctrl+←` / `Ctrl+→`  | move cursor by one word               |
| `Home` / `End`       | jump to start / end of line           |
| `Backspace` / `Del`  | edit                                  |
| `Ctrl+W`             | kill word backward                    |
| `Ctrl+U`             | kill from cursor to start of line     |
| `Ctrl+K`             | kill from cursor to end of line       |
| `Ctrl+C` / `Ctrl+V` / `Ctrl+X` | copy / paste / cut input via the system clipboard |
| `Ctrl+S`             | save the session (every cell input) to `~/.z_console_session_TIMESTAMP.z` |
| `Ctrl+=` / `Ctrl+-`  | grow / shrink font size               |
| `Ctrl+L` / `Cmd+L`   | clear all cells                       |
| Drop a `.z` file     | loads it into the prompt              |
| Drop an image file   | inserts `(ui:image "path")` at the cursor |
| Mouse wheel          | scroll output                         |
| Click ↓ button       | jump to the latest cell when scrolled up |

### Autocomplete

As soon as you start typing a symbol, a floating popup appears above the
input listing every special form, builtin, and user-defined variable that
starts with what you've typed. Each row shows the **name plus its call
shape** (parameter names like `(substring s start [end])`), so you can
see at a glance what arguments are expected. The matched prefix is
highlighted in a contrasting colour:

```
┌─────────────────────────────────────────────────────┐
│  print        (print ...values)                     │
│  pop          (pop array)                           │
│  pow          (pow base exponent)                   │
│  substring    (substring s start [end])             │
└─────────────────────────────────────────────────────┘
z> (p_
```

Signature data comes from a hand-authored table for built-ins. For
**user-defined functions** the popup introspects the actual parameter
list from `(fn name (params) body)`, so:

```
z> (fn greet (name greeting) (concat greeting ", " name))
z> (g
┌─────────────────────────────────────────────────────┐
│  greet        (greet name greeting)                 │
└─────────────────────────────────────────────────────┘
```

When the call shape isn't known (e.g., for a variable that holds a
non-function), the row falls back to the cheatsheet example.

Once you finish typing the function name and move on to its arguments (a
space, a string, another paren — whatever), the popup **stays on screen as
a single-row signature hint** showing that function's full example until
the call is closed or you press Enter:

```
┌──────────────────────────────────────────────┐
│  print        (print "Hello")                │
└──────────────────────────────────────────────┘
z> (print "hi, _
```

This way you can keep glancing at the expected call shape while you fill
in arguments. Tab/Enter only consume the popup when it's in completion
mode (multi-row); in hint mode they fall through to their normal jobs
(Enter runs the line, Tab is a no-op).

### Live error feedback

A thin amber strip appears just above the prompt while the input is
malformed, listing every static issue z-console can detect without
actually running the code:

```
  2 unclosed parens  ·  string not closed  ·  unknown: prnt
z> (prnt "hi
```

The checks are conservative: only call-head positions are scanned for
unknown identifiers (so `lambda` parameters and `set` targets don't false-
positive), and the strip disappears entirely once the input balances.

For example:

- typing `(a` shows `abs`, `acos`, `and`, `append`, `array`, `asin`,
  `assert`, `atan`, `atan2`, ...
- typing `(img` shows `img:add-text`, `img:barcode`, `img:bw`,
  `img:circle`, `img:compose`, `img:create`, `img:crop`, ... (when built
  with `IMAGE=1`)
- typing `(vision` shows the four `vision:*` builtins (with `VISION=1`)

Use `↑` / `↓` to walk the list, `Tab` or `Enter` to insert, `Esc` to
dismiss. The completion source is the **live env**, so functions and
variables you defined this session also appear.

History is shared with `z` and `zide` via `~/.z_history`.

## Quick taste

```lisp
(do
    (ui:rect    0  0 600 200 "#fff8e0")
    (ui:circle 80 80 50 "tomato")
    (ui:circle 200 80 50 "gold" )
    (ui:circle 320 80 50 "steelblue")
    (ui:line    0 150 600 150 "black")
    (ui:text-at 20 165 "drawn with z" 18 "black"))
```

Display an image inline:

```lisp
(ui:image "photo.png")               ; native size, top-left of canvas
(ui:image "photo.png" 10 10)         ; positioned at (10, 10), native size
(ui:image "photo.png" 0 0 320 240)   ; positioned and sized
(ui:image "photo.png" 0 0 320)       ; width 320, height auto (keeps aspect)
```

Combine with the image module to render anything you just generated:

```lisp
(img:create "out.png" 400 300 "white")
(img:circle "out.png" "out.png" 200 150 80 "tomato")
(ui:image  "out.png")                 ; show it right in the cell
```

A bar chart:

```lisp
(set values (array 30 80 120 50 95 140 70))
(set x 20)
(for v values
    (do
        (ui:rect x (- 180 v) 50 v "steelblue")
        (set x (+ x 60))))
```

Mix in any z code — JSON, file I/O, HTTP, regex — and visualise the result
inline. The full stdlib (`json:*`, `http:*`, `regex:*`, `url:*`, `zip:*`,
`base64:*`, `encrypt`, `uuid`, `scanf`, `input`, …) is available exactly as
in `z` and `zide`.
