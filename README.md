# z — Mini Script Language

# Objective
To make a small and functional script engine less than 5MB size.


A tiny Lisp-flavoured language and tree-walking interpreter written in C99.
No external dependencies beyond the standard library, and the same code
builds on **macOS**, **Linux**, and **Windows**.

Ships as two binaries:

- **`z`** — the interpreter; runs `.z` files and a basic REPL.
- **`zide`** — an enhanced REPL with live syntax highlighting, Tab
  autocompletion, and history (see [zide — enhanced REPL](#zide--enhanced-repl)).

Source files use the `.z` extension.

## Install

Windows users then run:

```
scoop bucket add z https://github.com/vivekg13186/scoop-z
scoop install z-lang
```


## Build

### macOS / Linux

```
make            # builds both z and zide
make test
```

`make` compiles both binaries into `dist/<os>_<arch>/` — e.g. `dist/linux_x86/z`
and `dist/linux_x86/zide` on Linux x86_64, `dist/macos_arm64/...` on Apple
Silicon, and so on. For convenience it also creates `./z` and `./zide`
symlinks at the project root pointing at the right platform folder, so you can
just run them from there:

```
./z examples/adults.z      # the interpreter
./zide                     # the enhanced REPL
make zide                  # build only zide
make info                  # prints platform, arch, and bin paths
```

You need a C compiler (`cc`/`gcc`/`clang`). On macOS run `xcode-select --install`
once if you've never done it. On Linux install `build-essential` (Debian/Ubuntu)
or the `gcc` package (Fedora/Arch).

### Windows

Two options.

**Option A — `build.bat`** (auto-detects MSVC or MinGW):

```
build.bat
z.exe examples\adults.z
zide.exe
```

Builds both `z.exe` and `zide.exe` into `dist\windows_x86\` and copies them to
the project root. Open a Developer Command Prompt if you want it to use MSVC's
`cl`. Open a normal shell with `gcc` on PATH if you'd rather use MinGW-w64.
Pass `build.bat --image` to enable the optional image module.

**Option B — CMake** (works with any generator: Visual Studio, Ninja, MinGW):

```
cmake -S . -B build
cmake --build build --config Release
build\Release\z.exe examples\adults.z
```

### CMake (any platform)

```
cmake -S . -B build
cmake --build build
./build/z examples/hello.z
```

 

## Run

```
z                # start an interactive REPL
z program.z      # run a source file
z --version      # print the version (e.g. `z 0.0.4`); -v also works
zide             # start the enhanced REPL
zide program.z   # zide also runs files
zide --version   # same flag on the enhanced REPL
```

The REPL banner shows the version on start:

```
$ z
z 0.0.4 — REPL.
  type `help` for a cheat sheet · arrows: history / cursor · Ctrl-D: exit · Ctrl-C: cancel
z>
```

`make` builds two binaries: `z` (the interpreter + basic REPL) and `zide`
(an enhanced REPL — see below). Both land in `dist/<os>_<arch>/`.

## zide — enhanced REPL

`zide` is a richer interactive front end built on the same interpreter:

- **Live syntax highlighting** as you type — comments, strings, numbers,
  special forms, builtins, `${...}` interpolation, and brackets are colourised.
- **Tab autocompletion** of special forms, builtins, and your own defined
  variables (completes the common prefix, lists options when ambiguous).
- **Arrow-key history** (shared `~/.z_history`), in-line editing
  (`←`/`→`, `Home`/`End`, `Ctrl-A/E/K/L`), and bracket-aware multi-line input.
- `help` / `?` for the cheat sheet, `:q` (or `:quit`) to exit.

```
make            # builds both z and zide
./zide          # launch it (root symlink → dist/<os>_<arch>/zide)
```

Results are shown with a `=>` prefix. Like `z`, `zide program.z` will just
run a file. When output isn't a terminal (piped/redirected) it degrades
gracefully to plain, uncoloured behaviour.

A quick session:

```
$ ./zide
zide — enhanced REPL for z
syntax colouring · Tab completes · arrows for history · `help` for the cheat sheet · :q to quit
zide> (set xs (array 1 2 3 4))
=> [1, 2, 3, 4]
zide> (map (lambda (n) (* n n)) xs)
=> [1, 4, 9, 16]
zide> :q
bye!
```

### REPL line editing

The REPL has a built-in line editor with command history:

| Key            | Action                                  |
| -------------- | --------------------------------------- |
| `↑` / `↓`      | Navigate previous / next history entry  |
| `←` / `→`      | Move cursor left / right                |
| `Home` / `End` | Jump to start / end of line             |
| `Ctrl-A` / `Ctrl-E` | Same as Home / End                 |
| `Ctrl-K`       | Delete from cursor to end of line       |
| `Ctrl-L`       | Clear the screen                        |
| `Ctrl-C`       | Cancel the current line                 |
| `Ctrl-D`       | Exit the REPL (on an empty line)        |

History persists across sessions in `~/.z_history` (or `%USERPROFILE%\.z_history` on Windows), up to 1000 entries.

### Built-in help

At the REPL, type `help` (or `?`) to see a categorized cheat sheet, or
`(help "topic")` for one section. Topics: `forms` `arith` `cmp` `logic`
`arrays` `strings` `regex` `math` `core` `file` `json` `http` `system`
(plus `image` when built with `IMAGE=1`).

## What's implemented


- **Values:** number, string, boolean, null, array, object, function
- **Syntax:** s-expressions, line comments with `;`, `[ ... ]` parsed the same as `( ... )`
- **Special forms:** `do`, `if`, `while`, `for`, `fn`, `lambda`, `set`, `try`/`catch`, `quote`, `&&` / `and`, `||` / `or`
- **Variables:** `set`, bare-symbol lookup, dotted access (`user.name`), `(get container key ...)` chain
- **Arithmetic:** `+ - * / %` (and `+` doubles as string concatenation when the first arg is a string)
- **Comparison:** `< > <= >= == !=`
- **Logic:** `&&` `||` `!` (also as `and` / `or`)
- **Arrays:** `array`, `get`, `put`, `push`, `pop`, `length`, `reverse`, `sort`, `chunk`, `map`, `filter`, `reduce`
- **Objects:** `object`, `get`, `put`, `keys`, `values`, `entries`
- **Strings:** `concat`, `split`, `join`, `trim`, `lower`, `upper`, `replace`, `substring`, `between`, `levenshtein`, `reverse`, `starts-with`, `ends-with`, `contains`, `index-of`
- **Template strings:** any `"..."` literal can embed `${expr}` — full z expressions evaluated in scope. Use `\$` for a literal `$`.
- **Regex:** `regex:test`, `regex:match`, `regex:find-all`, `regex:replace`, `regex:split` — supports `. * + ? ^ $ [...] [^...] \d \w \s \D \W \S`
- **Math:** `min`, `max`, `floor`, `ceil`, `round`, `trunc`, `abs`, `sign`, `mod`, `sqrt`, `cbrt`, `pow`, `exp`, `log`, `log2`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `random`; constants `pi`, `e`
- **I/O:** `print`, `read`, `read-lines`, `write`, `append`, `delete`, `list-dir`, `file-info`, `copy-file`, `move-file`
- **JSON:** `json:parse`, `json:stringify`
- **Encoding / crypto:** `base64:encode`, `base64:decode`, `encrypt`, `decrypt` (lightweight XTEA-CTR), `uuid` (v4), `md5`, `sha256`, `sha512`
- **URL:** `url:encode`, `url:decode`, `url:build` (object → query string)
- **Archives:** `zip:create`, `zip:extract` (need `zip`/`unzip`), `tar:create`, `tar:extract` (`gz`/`bz2`/`xz` compression)
- **Parsing / input:** `scanf` — `%d %f %s %c` and literal text → array of values; reads stdin when called with just a format. `input [prompt]` reads a line from stdin.
- **HTTP:** `http:get url [headers]`, `http:post url body [headers]` — headers is an object; delegates to `curl` (bundled with Windows 10 1803+, install via package manager elsewhere)
- **System:** `type`, `assert`, `sleep`, `now`, `timestamp`, `format-date`, `env`, `exec`, `run`, `argv`, `exit`, `import`, `help`
- **Errors:** `try`/`catch` with `setjmp`/`longjmp`

## Quick taste

```lisp
(do
    (set users
        (array
            (object "name" "Alice" "age" 20)
            (object "name" "Bob"   "age" 15)
        )
    )

    (for user users
        (if (> (get user "age") 18)
            (print (concat (get user "name") " is adult"))
        )
    )
)
```

## Running shell commands

```lisp
; Combined stdout+stderr as a string.
(exec "echo hello")        ; → "hello\n"

; Structured result, never throws — check the code yourself.
(run "ls -1")              ; → { "stdout": "...", "code": 0 }
(run "false")              ; → { "stdout": "",    "code": 1 }

; Pick up arguments the user passed to the z program:
;   z myscript.z foo bar
(argv)                     ; → ["foo", "bar"]
```

## Template strings and regex

```lisp
(set name "vivek")  (set yr 2026)

"hello ${name}"                    ; → "hello vivek"
"next year: ${(+ yr 1)}"           ; → "next year: 2027"
"len=${(length name)}"             ; → "len=5"
"price: \$${yr}"                   ; → "price: $2026"      (escape with \$)

(regex:test     "\d+" "answer is 42")              ; → true
(regex:match    "\d+" "answer is 42")              ; → "42"
(regex:find-all "\d+" "a1 b22 c333")               ; → ["1", "22", "333"]
(regex:replace  "\d+" "have 3 cats, 4 dogs" "***") ; → "have *** cats, *** dogs"
(regex:split    "[, ]+" "a, b,  c d")              ; → ["a", "b", "c", "d"]
```

Pattern syntax: `. * + ? ^ $ [class] [^class]` plus the shorthands `\d \w \s \D \W \S`.

## HTTP

```lisp
; Simple GET / POST — unchanged.
(http:get  "https://example.com")
(http:post "https://api.example.com/log" (object "level" "info" "msg" "hi"))

; Optional headers as an object on the last arg.
(http:get "https://api.example.com/users/42"
          (object "Authorization" "Bearer abc"
                  "Accept"        "application/json"
                  "X-Request-Id"  17))            ; numbers/booleans stringified

; POST with custom headers — Content-Type defaults to application/json
; for object bodies, text/plain for strings, but your override wins.
(http:post "https://api.example.com/upload"
           "row1,row2,row3"
           (object "Content-Type" "text/csv"
                   "X-Trace"      "abc"))
```

Bodies are written to a temp file and passed via `--data-binary @<file>`, so JSON with embedded quotes works on every shell. The temp file is deleted right after the call.

## Optional modules

Some functionality is gated behind compile-time flags so the core build
stays lean. Pass the flag at build time to opt in.

### Image manipulation — `IMAGE=1`

Builtins: `img:create`, `img:resize`, `img:crop`, `img:rotate`, `img:compose`, `img:replace-color`, `img:circle`, `img:rect`, `img:add-text`, `img:bw`, `img:grayscale`, `img:to-pdf`, `img:qr`, `img:barcode`, `img:info`.

`img:qr` and `img:barcode` use extra tools instead of ImageMagick:
`qrencode` for QR (`brew install qrencode` / `apt-get install qrencode`) and
`zint` for barcodes (`brew install zint` / `apt-get install zint`).

```
make IMAGE=1
# or
cmake -S . -B build -DZ_WITH_IMAGE=ON && cmake --build build
# Windows:
build.bat --image
```

Runtime requirement: **ImageMagick** must be on `PATH` (the module shells out
to `magick` or `convert`).

```
macOS:    brew install imagemagick
Linux:    sudo apt-get install imagemagick     # or dnf install ImageMagick
Windows:  scoop install imagemagick             # or the official MSI installer
```

Quick taste:

```lisp
(img:create   "canvas.png" 400 300 "white")
(img:circle   "canvas.png" "canvas.png" 200 150 60 "tomato")
(img:rect     "canvas.png" "canvas.png"  20 220 360 60 "#eef" "black" 2)
(img:info     "photo.jpg")                     ; → { width, height, format }
(img:resize   "photo.jpg" "thumb.png" 256 256)
(img:crop     "photo.jpg" "tile.png"  50 50 200 200)
(img:rotate   "photo.jpg" "tilted.png" 45)
(img:add-text "photo.jpg" "captioned.png" "hello" 20 20 32 "yellow")
(img:bw        "photo.jpg" "scan.png" 50)              ; 1-bit, 50% threshold
(img:grayscale "photo.jpg" "gray.png")                 ; 8-bit grayscale
(img:to-pdf    (array "p1.png" "p2.png" "p3.png") "doc.pdf")
(img:qr        "https://example.com" "qr.png" 6)       ; QR code
(img:barcode   "012345678905" "code.png" "ean13")      ; barcode
```

Colours are anything ImageMagick accepts — named (`"red"`), hex (`"#ff8800"`), RGB (`"rgb(0,128,255)"`), or `"none"` for transparency.

**PDF gotcha on Linux:** some distros ship ImageMagick with PDF write disabled by default. If `img:to-pdf` fails with "not authorized", edit `/etc/ImageMagick-6/policy.xml` (or `-7`) and change the `pattern="PDF"` policy from `rights="none"` to `rights="read|write"`.

If you call any `img:*` function without `IMAGE=1` set, you'll get a clean
"undefined variable" error — the module simply isn't registered.

### Computer vision — `VISION=1`

A small set of detection builtins for working with photos. Like the image
module it shells out to existing tools, so the build itself stays a single C
file. Each function probes for what it needs and reports a useful error if
the tool is missing.

Builtins: `vision:barcode`, `vision:faces`, `vision:objects`, `vision:plate`.

```
make VISION=1
# combine with image:
make IMAGE=1 VISION=1
```

Runtime requirements (only the tool you actually call):

| Function          | Tool             | Install |
| ----------------- | ---------------- | ------- |
| `vision:barcode`  | `zbarimg`        | `apt-get install zbar-tools` · `brew install zbar` |
| `vision:faces`    | `python3` + `opencv-python` | `pip install opencv-python` |
| `vision:objects`  | `python3` + `opencv-python` | (same) |
| `vision:plate`    | `alpr` (OpenALPR) | `apt-get install openalpr` · `brew install openalpr` |

Each function returns an **array** of detections — an empty array means "no
detections", not an error.

```lisp
(vision:barcode "receipt.png")
; → [ { "type": "QR-Code", "data": "https://example.com" } ]

(vision:faces   "group.jpg")
; → [ { "x": 120, "y": 60, "width": 80, "height": 80 } ]

(vision:objects "street.jpg")
; → [ { "class": "person", "confidence": 0.87,
;       "x": 100, "y": 200, "width": 64, "height": 128 } ]

(vision:plate   "car.jpg")
; → [ { "plate": "ABC1234", "confidence": 89.2 } ]
```

Object detection currently uses OpenCV's bundled HOG people-detector — no
model download required, but it only finds people. For richer detection
(`person`, `car`, `dog`, ...), drop in a YOLO/DNN call in `z_vision.h`.

## Portability notes

- A small platform layer at the top of `z.c` wraps the calls that differ
  between POSIX and Win32 (`nanosleep` vs `Sleep`, `clock_gettime` vs
  `GetSystemTimeAsFileTime`, `localtime_r` vs `localtime_s`, `popen` vs `_popen`).
- JSON stringification uses an in-tree growing string buffer rather than
  `open_memstream`, which isn't available on Windows.
- `_POSIX_C_SOURCE` / `_DEFAULT_SOURCE` are only defined on non-Windows builds.
- The binary depends only on the C standard library (`-lm` on POSIX). No third-party libraries.

## Design

- Single-file C99 implementation in `z.c`.
- Tree-walking interpreter: the parser builds `Value` trees that the evaluator
  walks directly. No bytecode or VM yet.
- Memory model is arena-style leak — fine for short   scripts. A GC is
  a natural next step.
- Errors propagate through `setjmp`/`longjmp` so `try`/`catch` works cleanly.
