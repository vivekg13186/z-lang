# z — Mini Lisp   Language

A tiny Lisp-flavoured language and tree-walking interpreter written in C99.
Single source file, no external dependencies beyond the standard library, and
the same code builds on **macOS**, **Linux**, and **Windows**.

Source files use the `.z` extension.

## Build

### macOS / Linux

```
make
make test
```

The compiled binary lands in `dist/<os>_<arch>/z`. So on Linux x86_64 it's
`dist/linux_x86/z`, on macOS arm64 it's `dist/macos_arm64/z`, etc. Both
`make test` and `make run` invoke it from there.

```
./dist/linux_x86/z examples/adults.z       # or whichever folder is yours
make info                                   # prints platform, arch, and bin path
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
```

Open a Developer Command Prompt if you want it to use MSVC's `cl`. Open a normal
shell with `gcc` on PATH if you'd rather use MinGW-w64.

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

### Docker

A multi-stage `Dockerfile` builds z, runs the test suite as a sanity
check, and produces a slim runtime image with the binary plus the
examples on board.

```
# Core build
docker build -t z .

# With the optional image module (also pulls in ImageMagick)
docker build --build-arg IMAGE=1 -t z .

# Run a script — mount your working directory at /work
docker run --rm -v "$PWD:/work" z program.z

# Or try a bundled example
docker run --rm z /opt/z/examples/adults.z

# Drop into the REPL
docker run --rm -it z
```

## Run

```
z                # start an interactive REPL
z program.z      # run a source file
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
- **Arrays:** `array`, `get`, `put`, `push`, `pop`, `length`, `map`, `filter`, `reduce`
- **Objects:** `object`, `get`, `put`, `keys`, `values`, `entries`
- **Strings:** `concat`, `split`, `trim`, `lower`, `upper`, `replace`, `substring`, `starts-with`, `ends-with`, `contains`, `index-of`
- **Template strings:** any `"..."` literal can embed `${expr}` — full z expressions evaluated in scope. Use `\$` for a literal `$`.
- **Regex:** `regex:test`, `regex:match`, `regex:find-all`, `regex:replace`, `regex:split` — supports `. * + ? ^ $ [...] [^...] \d \w \s \D \W \S`
- **Math:** `min`, `max`, `floor`, `ceil`, `abs`, `random`
- **I/O:** `print`, `read`, `write`, `append`, `delete`, `list-dir`, `file-info`
- **JSON:** `json:parse`, `json:stringify`
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

Builtins: `img:create`, `img:resize`, `img:crop`, `img:rotate`, `img:circle`, `img:rect`, `img:add-text`, `img:bw`, `img:grayscale`, `img:to-pdf`, `img:info`.

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
```

Colours are anything ImageMagick accepts — named (`"red"`), hex (`"#ff8800"`), RGB (`"rgb(0,128,255)"`), or `"none"` for transparency.

**PDF gotcha on Linux:** some distros ship ImageMagick with PDF write disabled by default. If `img:to-pdf` fails with "not authorized", edit `/etc/ImageMagick-6/policy.xml` (or `-7`) and change the `pattern="PDF"` policy from `rights="none"` to `rights="read|write"`.

If you call any `img:*` function without `IMAGE=1` set, you'll get a clean
"undefined variable" error — the module simply isn't registered.

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
