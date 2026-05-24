# z — Mini Lisp Workflow Language

A tiny Lisp-flavoured language and tree-walking interpreter written in C99.
Single source file, no external dependencies beyond the standard library, and
the same code builds on **macOS**, **Linux**, and **Windows**.

Source files use the `.z` extension.

## Build

### macOS / Linux

```
make
./z examples/adults.z
make test
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

## Run

```
z                # start an interactive REPL
z program.z      # run a source file
```

## What's implemented

Everything from the "Core + stdlib" scope of `Concept.md`:

- **Values:** number, string, boolean, null, array, object, function
- **Syntax:** s-expressions, line comments with `;`, `[ ... ]` parsed the same as `( ... )`
- **Special forms:** `do`, `if`, `while`, `for`, `fn`, `lambda`, `set`, `try`/`catch`, `quote`, `&&` / `and`, `||` / `or`
- **Variables:** `set`, bare-symbol lookup, dotted access (`user.name`), `(get container key ...)` chain
- **Arithmetic:** `+ - * / %` (and `+` doubles as string concatenation when the first arg is a string)
- **Comparison:** `< > <= >= == !=`
- **Logic:** `&&` `||` `!` (also as `and` / `or`)
- **Arrays:** `array`, `get`, `put`, `push`, `pop`, `length`, `map`, `filter`, `reduce`
- **Objects:** `object`, `get`, `put`, `keys`, `values`, `entries`
- **Strings:** `concat`, `split`, `trim`, `lower`, `upper`, `replace`, `substring`
- **Math:** `min`, `max`, `floor`, `ceil`, `abs`, `random`
- **I/O:** `print`, `read`, `write`, `append`, `delete`
- **JSON:** `json:parse`, `json:stringify`
- **HTTP:** `http:get`, `http:post` (delegates to `curl` — bundled with Windows 10 1803+, install via package manager elsewhere)
- **System:** `type`, `assert`, `sleep`, `now`, `timestamp`, `format-date`, `env`, `exec`, `run`, `argv`, `exit`, `import`
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
- Memory model is arena-style leak — fine for short workflow scripts. A GC is
  a natural next step.
- Errors propagate through `setjmp`/`longjmp` so `try`/`catch` works cleanly.
