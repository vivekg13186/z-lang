# z — Mini Script Language

# Objective
To make a small and functional script engine less than 5MB size.


A tiny Lisp-flavoured language and tree-walking interpreter written in C99.
No external dependencies beyond the standard library, and the same code
builds on **macOS**, **Linux**, and **Windows**.

Ships as two binaries:

- **`z`** — the interpreter; runs `.z` files and a basic REPL.
- **`zide`** — an enhanced terminal REPL with Jupyter-style numbered cells,
  live syntax highlighting, bracket-match highlight, inline ghost-text
  signatures, a dropdown Tab popup, history, and a `:save` command that
  writes every cell to a `.z` file (see [zide — enhanced REPL](#zide--enhanced-repl)).
- **`z-console`** — an optional raylib-based GUI REPL with the same
  feature set plus inline images, themes, mouse selection, and a
  Ctrl+S session save. Builds out of the `z-console/` folder.
  Needs raylib — on Ubuntu < 24.04 (or any distro without
  `libraylib-dev`) run `./scripts/install-raylib.sh` to build it from
  source in one shot.

Source files use the `.z` extension.

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

`zide` is a richer interactive front end built on the same interpreter.
Designed to feel like a notebook in your terminal:

- **Numbered cells** — `In [N]>` prompts with `Out [N]=` results, a thin
  divider between cells, and a `   ...   ` continuation prompt while a
  multi-line expression is still open.
- **Live syntax highlighting** as you type — comments, strings, numbers,
  special forms, builtins, `${...}` interpolation, and brackets.
- **Bracket-match highlight** — when the caret sits next to a `(`, `)`,
  `[`, or `]`, the bracket and its partner light up in inverse yellow.
  Skips brackets inside strings and `;` comments.
- **Inline signature ghost-text** — type `(funcname ` (any documented
  builtin) and the rest of the signature plus its one-line description
  is shown as dim ghost text after the caret, automatically disappearing
  as you keep typing. Signatures are pulled from the same help topics
  that `(help)` prints.
- **Autocomplete popup** — Tab opens a dropdown beneath the prompt
  listing every match (up to 7 visible rows) with each row's signature
  and description. `↑`/`↓` to navigate, `Tab`/`Enter` to accept, `Esc`
  or any other key cancels (the cancelling key is re-fed into the editor
  so flow doesn't break).
- **Save a session as a `.z` file** — type `:save` to dump every
  committed cell to `~/.zide_session_YYYYMMDD_HHMMSS.z`, or
  `:save path/to/file.z` to pick the path. The file is a normal `.z`
  script: each cell separated by a blank line, with a header comment.
- **Arrow-key history** (shared `~/.z_history`), in-line editing
  (`←`/`→`, `Home`/`End`, `Ctrl-A/E/K/L`), and bracket-aware multi-line input.
- `help` / `?` for the cheat sheet, `:q` (or `:quit`) to exit.

```
make            # builds both z and zide
./zide          # launch it (root symlink → dist/<os>_<arch>/zide)
```

Like `z`, `zide program.z` runs a file and exits. When output isn't a
terminal (piped/redirected) it degrades gracefully to plain, uncoloured
behaviour.

A quick session:

```
$ ./zide
zide 0.0.4 — enhanced REPL for z
numbered cells · bracket match · Tab popup · :save [path] writes cells to .z · :q to quit
─────────────────────────────────────────────────────────────────
In [1]> (set xs (array 1 2 3 4))
Out [1]= [1, 2, 3, 4]
─────────────────────────────────────────────────────────────────
In [2]> (map (lambda (n) (* n n)) xs)
Out [2]= [1, 4, 9, 16]
─────────────────────────────────────────────────────────────────
In [3]> :save /tmp/squares.z
; saved 2 cells → /tmp/squares.z
─────────────────────────────────────────────────────────────────
In [3]> :q
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
| `Tab`          | Autocomplete — opens a dropdown popup when ambiguous |
| `Ctrl-C`       | Cancel the current line                 |
| `Ctrl-D`       | Exit the REPL (on an empty line)        |

REPL commands (typed at any prompt):

| Command          | Action                                            |
| ---------------- | ------------------------------------------------- |
| `help` / `?`     | Show the cheat sheet (same as `(help)`)           |
| `:save`          | Save every cell so far to `~/.zide_session_*.z`   |
| `:save <path>`   | Same, to an explicit path                         |
| `:q` / `:quit`   | Exit                                              |

History persists across sessions in `~/.z_history` (or `%USERPROFILE%\.z_history` on Windows), up to 1000 entries.

### Built-in help

At the REPL, type `help` (or `?`) to see a categorized cheat sheet, or
`(help "topic")` for one section. Topics: `forms` `arith` `cmp` `logic`
`arrays` `strings` `regex` `math` `core` `file` `json` `http` `system`
`html` (plus `image` when built with `IMAGE=1`, `vision` with `VISION=1`,
`sqlite` with `SQLITE=1`, `ocr` with `OCR=1`, `cv` with `CV=1`). The
http topic reflects how the build links — "via curl" by default,
"via libcurl" when built with `LIBCURL=1`.

## What's implemented


- **Values:** number, string, **bytes** (binary-safe, may contain NUL), boolean, null, array, object, function
- **Syntax:** s-expressions, line comments with `;`, `[ ... ]` parsed the same as `( ... )`
- **Number literals:** decimal (`42`, `3.14`, `1.5e-3`), hex (`0xff`, `0XFF`), binary (`0b1010`), all optionally signed; `_` may appear anywhere in a numeric literal as a separator (`0xdead_beef`, `1_000_000`)
- **Special forms:** `do`, `if`, `when`, `unless`, `cond`, `let`, `->`, `->>`, `while`, `for`, `fn` (with `& rest` for variadic), `lambda`, `set`, `try`/`catch`, `quote`, `&&` / `and`, `||` / `or`
- **Variables:** `set`, bare-symbol lookup, dotted access (`user.name`), `(get container key ...)` chain. `set` also destructures: `(set (a b c) source)` pulls positionally when `source` is an array and by-key when it's an object.
- **Arithmetic:** `+ - * / %` (and `+` doubles as string concatenation when the first arg is a string)
- **Comparison:** `< > <= >= == !=`
- **Logic:** `&&` `||` `!` (also as `and` / `or`)
- **Arrays:** `array`, `get`, `put`, `push`, `pop`, `length`, `reverse`, `sort`, `chunk`, `map`, `filter`, `reduce`, `take`, `drop`, `take-while`, `drop-while`, `distinct`, `zip`, `group-by`, `merge`
- **Objects:** `object`, `get`, `put`, `keys`, `values`, `entries`, `merge`, `dissoc`, `select-keys`, `update`, `get-in`, `assoc-in`, `update-in`
- **Strings:** `concat`, `split`, `join`, `trim`, `lower`, `upper`, `replace`, `substring`, `between`, `levenshtein`, `reverse`, `starts-with`, `ends-with`, `contains`, `index-of`, `format`, `pad-left`, `pad-right`, `repeat`, `count-occurrences`, `slugify`
- **Template strings:** any `"..."` literal can embed `${expr}` — full z expressions evaluated in scope. Use `\$` for a literal `$`.
- **Regex:** `regex:test`, `regex:match`, `regex:find-all`, `regex:replace`, `regex:split` — supports `. * + ? ^ $ [...] [^...] \d \w \s \D \W \S`
- **Math:** `min`, `max`, `floor`, `ceil`, `round`, `trunc`, `abs`, `sign`, `mod`, `clamp`, `lerp`, `is-nan`, `is-finite`, `sqrt`, `cbrt`, `pow`, `exp`, `log`, `log2`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `random`, `random-int`, `random-choice`, `shuffle`, `random-seed`; constants `pi`, `e`, `inf`, `ninf`, `nan`
- **I/O:** `print`, `read`, `read-lines`, `read-bytes`, `write`, `write-bytes`, `append`, `delete`, `list-dir`, `file-info`, `copy-file`, `move-file`
- **Bytes:** `bytes`, `hex`, `unhex`, `bytes:get`, `bytes:slice`, `bytes:concat`, `string->bytes`, `bytes->string`
- **JSON:** `json:parse`, `json:stringify`
- **Encoding / crypto:** `base64:encode`, `base64:decode`, `encrypt`, `decrypt` (lightweight XTEA-CTR), `uuid` (v4), `md5`, `sha256`, `sha512`
- **URL:** `url:encode`, `url:decode`, `url:build` (object → query string)
- **Archives:** `zip:create`, `zip:extract` (need `zip`/`unzip`), `tar:create`, `tar:extract` (`gz`/`bz2`/`xz` compression)
- **HTML / XML:** `html:query`, `html:text`, `html:attr`, `xml:query`, `xml:text`, `xml:attr` — built-in CSS-selector subset (`tag`, `.class`, `#id`, `[attr]`, `[attr=v]`, `[attr*=v]`, `[attr^=v]`, `[attr$=v]`, descendant + direct-child combinators) and `/a/b/c`-style XPath
- **Parsing / input:** `scanf` — `%d %f %s %c` and literal text → array of values; reads stdin when called with just a format. `input [prompt]` reads a line from stdin.
- **HTTP:** `http:get url [headers] [opts]`, `http:post url body [headers] [opts]` — headers + opts are objects. `opts` keys: `verify-ssl`, `follow-redirects`, `max-redirects`, `timeout`, `user-agent`. Default build shells out to `curl` (bundled with Windows 10 1803+); build with `LIBCURL=1` to link libcurl directly and drop the binary dependency. Env `Z_HTTP_INSECURE=1` forces `verify-ssl` off globally.
- **Dates:** `now`, `timestamp`, `format-date`, `parse-date`, `date+`, `date-diff`
- **CSV:** `csv:parse`, `csv:stringify` (handles quoted fields with embedded commas / quotes / CRLF)
- **System:** `type`, `assert`, `sleep`, `env`, `exec`, `run`, `argv`, `exit`, `import`, `help`
- **Errors:** `try`/`catch` with `setjmp`/`longjmp`
- **Tail-call optimization:** calls in tail position — including self-recursion, mutual recursion through `if`/`cond`/`when`/`unless`/`let`/`do`/`and`/`or` — reuse the same C stack frame, so `(count-down 1_000_000)` and `(even? n)`/`(odd? n)` work for arbitrary depths.
- **Optional modules:** `IMAGE=1` (img:* via ImageMagick), `VISION=1` (vision:* barcode/faces/objects shellouts), `SQLITE=1` (sqlite:* / kv:* — links libsqlite3), `OCR=1` (ocr:image / ocr:words — links libtesseract+leptonica), `CV=1` (`cv:faces` — embedded Haar-cascade detector in pure C, no Python at runtime), `LIBCURL=1` (link libcurl directly instead of shelling out to the `curl` binary). All flags compose: `make IMAGE=1 OCR=1 SQLITE=1 LIBCURL=1`.

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

## Destructuring assignment

`set` accepts a list of symbol slots as its target, so you can pull
multiple values out of an array or object in one go — similar to
Python's `a, b, c = obj`:

```lisp
; positional from an array — missing slots become null
(set (a b c) (array 10 20 30))         ; a=10, b=20, c=30
(set [x y z] (array 1 2 3))            ; same; [ ] form parses to ( )
(set (p q r) (array 7 8))              ; p=7, q=8, r=null

; by-key from an object — missing keys become null
(set (name age role) (object "name" "Vivek" "age" 30 "role" "dev"))
(set (h i missing)  (object "h" 1 "i" 2))   ; h=1, i=2, missing=null

; combine with a function that returns multiple values via an array
(fn min-max (xs) (array (reduce min xs) (reduce max xs)))
(set (lo hi) (min-max (array 4 1 9 3 7)))    ; lo=1, hi=9
```

Every slot must be a symbol; the source must be an array or object —
anything else raises `set: destructure source must be array or object`.
Existing variables are rebound in place (same scoping rules as plain
`(set name value)`).

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
; for object bodies, text/plain for strings, application/octet-stream for
; bytes; your override wins.
(http:post "https://api.example.com/upload"
           "row1,row2,row3"
           (object "Content-Type" "text/csv"
                   "X-Trace"      "abc"))

; Options object — LAST arg, after headers. Use `null` for no headers.
(http:get "https://self-signed.example/"
          null
          (object "verify-ssl" false        ; skip cert + hostname checks
                  "timeout"    10           ; seconds
                  "follow-redirects" true
                  "user-agent" "my-bot/1"))

; Env-var override: Z_HTTP_INSECURE=1 forces verify-ssl false everywhere.
```

By default `http:*` shells out to the `curl` binary. POST bodies are written to a temp file and passed via `--data-binary @<file>`, so JSON with embedded quotes works on every shell; the temp file is deleted right after the call.

Build with `make LIBCURL=1` (also `LIBCURL=1` for `z-console`) to link **libcurl** directly — no `curl` binary required, no process spawn per call, cleaner error messages. The Makefile auto-discovers libcurl via `pkg-config libcurl` → `brew --prefix curl` → bare `-lcurl`. Install:

```
apt-get install libcurl4-openssl-dev
dnf install libcurl-devel
brew install curl
```

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

### SQLite + KV — `SQLITE=1`

Links against `libsqlite3` directly (unlike image/vision which shell out),
because SQLite is small, ubiquitous, and the wire-marshalling pain of a
CLI bridge isn't worth it.

Builtins: `sqlite:open`, `sqlite:exec`, `sqlite:query`, `sqlite:close`,
`sqlite:last-insert-id`, `kv:open`, `kv:set`, `kv:get`, `kv:del`,
`kv:keys`.

```
make SQLITE=1
# combine:
make IMAGE=1 VISION=1 SQLITE=1
```

Install the dev headers:

| OS      | How |
| ------- | --- |
| macOS   | `brew install sqlite`                                |
| Debian / Ubuntu | `sudo apt-get install libsqlite3-dev`        |
| Fedora  | `sudo dnf install sqlite-devel`                      |
| Arch    | `sudo pacman -S sqlite`                              |
| Windows | `pacman -S mingw-w64-x86_64-sqlite3` (MSYS2)         |

Cell type ↔ z mapping is total: `NULL`/`INTEGER`/`REAL`/`TEXT`/`BLOB` ↔
`null`/`number`/`number`/`string`/`bytes`. BLOBs use z's first-class
`bytes` type, so binary columns round-trip without NUL-truncation.

```lisp
(set db (sqlite:open ":memory:"))
(sqlite:exec  db "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INT)")
(sqlite:exec  db "INSERT INTO users (name, age) VALUES (?, ?)"
                 (array "Ada" 36))
(sqlite:query db "SELECT * FROM users WHERE age > ?" (array 30))
; → [{ "id": 1, "name": "Ada", "age": 36 }]
(sqlite:close db)

; KV wrapper — schema is created on open.
(set s (kv:open "settings.db"))
(kv:set s "theme" "dark")
(kv:get s "theme")        ; → "dark"
(kv:keys s "th")          ; → ["theme"]
```

### Embedded face detection — `CV=1`

Pure-C Haar cascade detector. No OpenCV link, no Python at runtime.
Combine with `IMAGE=1` so any JPG/PNG can be funnelled through
`(img:grayscale)` into the PGM format the detector reads.

```sh
make IMAGE=1 CV=1
```

One-time setup — grab a Haar cascade XML and convert it to z's compact
binary format:

```sh
# any OpenCV cascade XML works; this is the frontalface one
curl -L -o face.xml \
  https://raw.githubusercontent.com/opencv/opencv/4.x/data/haarcascades/haarcascade_frontalface_default.xml
python3 tools/cascade_to_bin.py face.xml face.zhc
```

Then in z:

```lisp
(img:grayscale "photo.jpg" "photo.pgm")            ; needs IMAGE=1
(cv:faces "photo.pgm" "face.zhc")
;  → [ { "x": 120, "y": 60, "width": 80, "height": 80, "score": 1.4 } ]

;; tune via opts:
(cv:faces "photo.pgm" "face.zhc"
          (object "scale-factor" 1.15
                  "min-size"     40
                  "max-size"     400
                  "merge"        true))
```

Cascade files are cached in-process by absolute path — calling
`cv:faces` repeatedly with the same `.zhc` is cheap. Multi-scale
detection with a 5% sliding step and non-max suppression (IoU > 0.3).
Algorithm is standard Viola-Jones with per-window variance
normalisation — same idea OpenCV uses, ~500 LOC.

### OCR — `OCR=1`

Embeds tesseract via its C API (no Python required at runtime). Adds
`ocr:image` and `ocr:words` builtins.

```sh
make OCR=1
```

Install the dev headers + a runtime tessdata language pack:

| OS              | How |
| --------------- | --- |
| macOS           | `brew install tesseract` |
| Debian / Ubuntu | `sudo apt-get install libtesseract-dev libleptonica-dev tesseract-ocr` |
| Fedora          | `sudo dnf install tesseract-devel leptonica-devel tesseract` |

```lisp
(ocr:image "doc.png")
;  → "the recognized text\n"

(ocr:image "doc.png" "eng+deu")
;  → mixed-language

(ocr:words "doc.png")
;  → [ { "word": "Hello", "confidence": 96.4,
;        "x": 12, "y": 30, "width": 64, "height": 22 }, ... ]
```

If you installed tessdata to a non-default path, point at it:

```sh
TESSDATA_PREFIX=/opt/homebrew/share/tessdata ./z myscript.z
```

**Header discovery**: the Makefile auto-locates both tesseract and
leptonica. Resolution cascade:

1. `pkg-config tesseract` (Linux + most Homebrew installs)
2. `pkg-config lept` or `pkg-config leptonica` separately — sometimes
   tesseract's `.pc` doesn't declare leptonica as a `Requires:`
3. `brew --prefix tesseract` + `brew --prefix leptonica` (macOS fallback)
4. Plain `-ltesseract -lleptonica` (assume default paths)

`z_ocr.h` also handles two leptonica header layouts via `__has_include`:
modern installs put it at `<leptonica/allheaders.h>`, older / unusual
ones at `<allheaders.h>` — either works.

If `make OCR=1` still fails:

```sh
brew install pkg-config        # makes step 1 work
# or override manually:
make OCR=1 \
  CFLAGS+="-I$(brew --prefix tesseract)/include -I$(brew --prefix leptonica)/include" \
  LDLIBS+="-L$(brew --prefix tesseract)/lib -L$(brew --prefix leptonica)/lib"
```

### Computer vision — `VISION=1`

A small set of detection builtins for working with photos. Like the image
module it shells out to existing tools, so the build itself stays a single C
file. Each function probes for what it needs and reports a useful error if
the tool is missing.

Builtins: `vision:barcode`, `vision:faces`, `vision:objects`. (License-plate OCR was removed in favour of building it from `OCR=1`'s `ocr:image` plus your own region proposals.)

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
