# z Language Cheat Sheet

Every command with a one-line example. Run any of these in the REPL (`./z` or
`./zide`) or in a `.z` file.

## Special forms

| Command | Example |
| --- | --- |
| `do` | `(do (print "a") (print "b"))` |
| `if` | `(if (> x 0) "pos" "neg")` |
| `when` | `(when (> x 0) (print "+"))` |
| `unless` | `(unless ok (print "fail"))` |
| `cond` | `(cond ((> x 0) "+") ((< x 0) "-") (else "0"))` |
| `let` | `(let ((x 10) (y 20)) (+ x y))` — `let*` semantics |
| `->` | `(-> "  hi  " trim upper)` — thread-first |
| `->>` | `(->> xs (map sq) (filter pos?))` — thread-last |
| `fn` (variadic) | `(fn log (level & msgs) ...)` — `&` rest collects remaining args |
| `while` | `(while (< i 10) (set i (+ i 1)))` |
| `for` | `(for x (array 1 2 3) (print x))` |
| `fn` | `(fn add (a b) (+ a b))` |
| `lambda` | `(lambda (x) (* x 2))` |
| `set` | `(set name "vivek")` |
| `try` / `catch` | `(try (/ 1 0) (catch e (print e)))` |
| `quote` | `(quote (a b c))` |
| `and` | `(and true false)` |
| `or` | `(or false true)` |

## Number literals

| Form | Example | Notes |
| --- | --- | --- |
| Decimal | `42` · `3.14` · `1.5e-3` | IEEE-754 double |
| Hex | `0xff` · `0XFF` · `0xdead_beef` | underscore allowed |
| Binary | `0b1010` · `0b1111_0000` | underscore allowed |
| Signed | `-0xff` · `+0b11` | sign attaches tightly |
| Separators | `1_000_000` · `3.141_592` | `_` is ignored anywhere in a numeric literal |

## Arithmetic

| Command | Example |
| --- | --- |
| `+` | `(+ 1 2 3)` |
| `-` | `(- 10 3)` |
| `*` | `(* 4 5)` |
| `/` | `(/ 20 4)` |
| `%` | `(% 10 3)` |

## Comparison

| Command | Example |
| --- | --- |
| `<` | `(< 1 2)` |
| `>` | `(> 2 1)` |
| `<=` | `(<= 2 2)` |
| `>=` | `(>= 3 2)` |
| `==` | `(== 5 5)` |
| `!=` | `(!= 5 6)` |

## Logic

| Command | Example |
| --- | --- |
| `&&` / `and` | `(&& true true)` |
| `\|\|` / `or` | `(or false true)` |
| `!` | `(! false)` |

## Arrays & objects

| Command | Example |
| --- | --- |
| `array` | `(array 1 2 3)` |
| `object` | `(object "name" "vivek" "age" 30)` |
| `get` | `(get (array 10 20) 1)` |
| `put` | `(put (object) "key" "val")` |
| `push` | `(push (array 1 2) 3)` |
| `pop` | `(pop (array 1 2 3))` |
| `length` | `(length "hello")` |
| `keys` | `(keys (object "a" 1))` |
| `values` | `(values (object "a" 1))` |
| `entries` | `(entries (object "a" 1))` |
| `reverse` | `(reverse (array 1 2 3))` → `[3, 2, 1]`; also reverses strings |
| `sort` | `(sort (array 3 1 2))` → `[1, 2, 3]`; pass `(fn x y)` cmp for custom order |
| `chunk` | `(chunk (array 1 2 3 4 5) 2)` → `[[1, 2], [3, 4], [5]]` |
| `take` / `drop` | `(take 3 arr)` · `(drop 3 arr)` |
| `take-while` / `drop-while` | predicate-based prefix/suffix |
| `distinct` | dedupe preserving order |
| `zip` | `(zip (array 1 2 3) (array "a" "b" "c"))` |
| `group-by` | `(group-by (lambda (n) (mod n 2)) arr)` → object |
| `merge` | `(merge o1 o2)` (object) or `(merge a1 a2)` (array concat) |
| `dissoc` | `(dissoc o "k")` — object minus key |
| `select-keys` | `(select-keys o (array "a" "b"))` |
| `update` | `(update o "n" (lambda (x) (+ x 1)))` |
| `get-in` / `assoc-in` / `update-in` | nested path traversal: `(get-in o (array "user" "name"))` |
| `map` | `(map (lambda (x) (* x x)) (array 1 2 3))` |
| `filter` | `(filter (lambda (x) (> x 2)) (array 1 2 3))` |
| `reduce` | `(reduce (lambda (a b) (+ a b)) (array 1 2 3) 0)` |

## Strings

| Command | Example |
| --- | --- |
| `concat` | `(concat "foo" "bar")` |
| `split` | `(split "," "a,b,c")` |
| `join` | `(join ", " (array 1 2 3))` → `"1, 2, 3"` |
| `trim` | `(trim "  hi  ")` |
| `lower` | `(lower "ABC")` |
| `upper` | `(upper "abc")` |
| `replace` | `(replace "hello" "l" "L")` |
| `substring` | `(substring "hello" 0 3)` |
| `between` | `(between "<a>hi</a>" "<a>" "</a>")` → `"hi"` |
| `reverse` | `(reverse "hello")` → `"olleh"` (also reverses arrays) |
| `levenshtein` | `(levenshtein "kitten" "sitting")` → `3` |
| `starts-with` | `(starts-with "hello" "he")` |
| `ends-with` | `(ends-with "hello" "lo")` |
| `contains` | `(contains "hello" "ell")` |
| `index-of` | `(index-of "hello" "l")` |
| `format` | `(format "%.2f%%" 42.5)` → `"42.50%"`; supports `%s %d %f %x %o %b %c %.Nf` |
| `pad-left` / `pad-right` | `(pad-left "x" 5 "0")` → `"0000x"` |
| `repeat` | `(repeat "ab" 3)` → `"ababab"` |
| `count-occurrences` | `(count-occurrences "banana" "a")` → `3` |
| `slugify` | `(slugify "Hello, World!")` → `"hello-world"` |
| template string | `"hi ${name}"` |

## Regex

| Command | Example |
| --- | --- |
| `regex:test` | `(regex:test "\d+" "abc123")` |
| `regex:match` | `(regex:match "\d+" "abc123")` |
| `regex:find-all` | `(regex:find-all "\d+" "a1 b2")` |
| `regex:replace` | `(regex:replace "\d" "a1b2" "X")` |
| `regex:split` | `(regex:split "[, ]+" "a, b c")` |

## Math

| Command | Example |
| --- | --- |
| `min` | `(min 3 1 2)` |
| `max` | `(max 3 1 2)` |
| `floor` | `(floor 3.7)` |
| `ceil` | `(ceil 3.2)` |
| `abs` | `(abs -5)` |
| `round` / `trunc` | `(round 3.5)` → `4` · `(trunc -3.7)` → `-3` |
| `sign` / `mod` | `(sign -3)` → `-1` · `(mod 10 3)` → `1` |
| `sqrt` / `cbrt` / `pow` | `(sqrt 2)` · `(pow 2 10)` → `1024` |
| `exp` / `log` / `log2` / `log10` | `(log 2.71828)` ≈ `1` |
| `sin` / `cos` / `tan` | radians: `(sin (/ pi 2))` → `1` |
| `asin` / `acos` / `atan` / `atan2` | `(atan2 1 1)` ≈ `pi/4` |
| `sinh` / `cosh` / `tanh` | `(tanh 0)` → `0` |
| `pi` / `e` | constants — `pi` ≈ `3.14159` |
| `random` | `(random)` |
| `random-int` / `random-choice` / `shuffle` / `random-seed` | `(random-int 1 6)` · `(random-choice arr)` · `(shuffle arr)` · `(random-seed 42)` |
| `clamp` / `lerp` | `(clamp x 0 1)` · `(lerp 0 100 0.25)` |
| `is-nan` / `is-finite` | bool predicates |
| `inf` / `ninf` / `nan` | constants |

## Core

| Command | Example |
| --- | --- |
| `print` | `(print "Hello")` |
| `type` | `(type 42)` |
| `assert` | `(assert (> 1 0) "must be positive")` |
| `sleep` | `(sleep 0.5)` |
| `help` | `(help "strings")` |

## File I/O

| Command | Example |
| --- | --- |
| `read` | `(read "data.txt")` |
| `read-lines` | `(read-lines "data.txt")` → array of lines |
| `read-bytes` | `(read-bytes "image.png")` → bytes (binary-safe; no NUL truncation) |
| `write-bytes` | `(write-bytes "out.bin" (bytes (array 0 1 2)))` |
| `bytes` | `(bytes (array 0 1 2 255))` · `(bytes "abc")` |
| `hex` / `unhex` | `(hex (bytes "z"))` → `"7a"` · `(unhex "de:ad")` → bytes |
| `bytes:get/slice/concat` | byte indexing, half-open slice, variadic concat |
| `string->bytes` / `bytes->string` | explicit conversion |
| `write` | `(write "out.txt" "content")` |
| `append` | `(append "log.txt" "a line")` |
| `delete` | `(delete "tmp.txt")` |
| `list-dir` | `(list-dir ".")` |
| `file-info` | `(file-info "z.c")` |
| `copy-file` | `(copy-file "a.txt" "b.txt")` |
| `move-file` | `(move-file "a.txt" "b.txt")` |

## JSON

| Command | Example |
| --- | --- |
| `json:parse` | `(json:parse "{\"a\":1}")` |
| `json:stringify` | `(json:stringify (object "a" 1))` |

## Encoding & crypto

| Command | Example |
| --- | --- |
| `base64:encode` | `(base64:encode "hello")` |
| `base64:decode` | `(base64:decode "aGVsbG8=")` |
| `encrypt` | `(encrypt "key" "secret")` |
| `decrypt` | `(decrypt "key" cipher)` |
| `uuid` | `(uuid)` |
| `md5` | `(md5 "abc")` → `"900150983cd24fb0d6963f7d28e17f72"` |
| `sha256` | `(sha256 "abc")` |
| `sha512` | `(sha512 "abc")` |

## URL

| Command | Example |
| --- | --- |
| `url:encode` | `(url:encode "hello world!")` |
| `url:decode` | `(url:decode "hello%20world%21")` |
| `url:build` | `(url:build "https://x.com/p" (object "q" "1 2"))` |

## Archives

| Command | Example |
| --- | --- |
| `zip:create` | `(zip:create "out.zip" (array "a.txt" "b.txt"))` |
| `zip:extract` | `(zip:extract "out.zip" "./dest")` |
| `tar:create` | `(tar:create "out.tgz" (array "a" "b") "gz")` |
| `tar:extract` | `(tar:extract "out.tgz" "./dest")` |

## Parsing

| Command | Example |
| --- | --- |
| `scanf` (string) | `(scanf "name=%s age=%d" "name=vivek age=30")` |
| `scanf` (stdin) | `(scanf "%d %s")` — reads one line from stdin |
| `input` | `(input "your name: ")` |

## HTTP

| Command | Example |
| --- | --- |
| `http:get` | `(http:get "https://example.com")` |
| `http:get` + headers | `(http:get url (object "Authorization" "Bearer x"))` |
| `http:post` | `(http:post url (object "k" "v"))` |
| `http:post` + headers | `(http:post url body (object "X-Trace" "abc"))` |

## System

| Command | Example |
| --- | --- |
| `now` | `(now)` |
| `timestamp` | `(timestamp)` |
| `format-date` | `(format-date (timestamp) "%Y-%m-%d")` |
| `env` | `(env "HOME")` |
| `exec` | `(exec "ls -1")` |
| `run` | `(run "ls")` |
| `argv` | `(argv)` |
| `exit` | `(exit 0)` |
| `import` | `(import "lib.z")` |

## Images (optional — build with `IMAGE=1`)

| Command | Example |
| --- | --- |
| `img:create` | `(img:create "c.png" 400 300 "white")` |
| `img:info` | `(img:info "in.png")` |
| `img:resize` | `(img:resize "in.png" "out.png" 200 200)` |
| `img:crop` | `(img:crop "in.png" "out.png" 0 0 100 100)` |
| `img:rotate` | `(img:rotate "in.png" "out.png" 90)` |
| `img:compose` | `(img:compose "bg.png" "logo.png" "out.png" 20 30)` |
| `img:replace-color` | `(img:replace-color "in.png" "out.png" "red" "blue" 10)` |
| `img:circle` | `(img:circle "c.png" "c.png" 100 100 50 "red")` |
| `img:rect` | `(img:rect "c.png" "c.png" 10 10 80 40 "blue")` |
| `img:add-text` | `(img:add-text "in.png" "out.png" "hi" 10 30 24 "white")` |
| `img:bw` | `(img:bw "in.png" "out.png")` |
| `img:grayscale` | `(img:grayscale "in.png" "out.png")` |
| `img:to-pdf` | `(img:to-pdf (array "a.png" "b.png") "out.pdf")` |
| `img:qr` | `(img:qr "https://example.com" "qr.png" 6)` |
| `img:barcode` | `(img:barcode "012345678905" "code.png" "ean13")` |

## Vision (optional — build with `VISION=1`)

Each function returns an array of detections. Empty array = no detections, not an error.

| Command | Example | Needs |
| --- | --- | --- |
| `vision:barcode` | `(vision:barcode "receipt.png")` → `[{type, data}]` | `zbarimg` |
| `vision:faces` | `(vision:faces "group.jpg")` → `[{x, y, width, height}]` | `python3` + `opencv-python` |
| `vision:objects` | `(vision:objects "street.jpg")` → `[{class, confidence, x, y, width, height}]` | `python3` + `opencv-python` |

## Date math

| Command | Example |
| --- | --- |
| `parse-date` | `(parse-date "2024-05-12 10:00:00")` → ms-since-epoch |
| `date+` | `(date+ ts 7 "days")` — units: ms / seconds / minutes / hours / days / weeks |
| `date-diff` | `(date-diff a b "hours")` |
| `format-date` | `(format-date (/ ts 1000) "%Y-%m-%d")` |

## CSV

| Command | Example |
| --- | --- |
| `csv:parse` | `(csv:parse "a,b\n\"with, comma\",3")` → `[["a","b"],["with, comma","3"]]` |
| `csv:stringify` | `(csv:stringify rows)` — quotes fields that contain `, " \n` |

## SQLite + KV (optional — build with `SQLITE=1`)

| Command | Example |
| --- | --- |
| `sqlite:open` | `(sqlite:open ":memory:")` or `(sqlite:open "app.db")` |
| `sqlite:exec` | `(sqlite:exec db "INSERT INTO t (x) VALUES (?)" (array 1))` → affected rows |
| `sqlite:query` | `(sqlite:query db "SELECT * FROM t WHERE x > ?" (array 0))` → array of row-objects |
| `sqlite:close` | `(sqlite:close db)` |
| `sqlite:last-insert-id` | `(sqlite:last-insert-id db)` |
| `kv:open` | `(kv:open "store.db")` — creates `kv (k, v)` schema on first open |
| `kv:set` / `kv:get` / `kv:del` | `(kv:set s "k" 42)` · `(kv:get s "k")` · `(kv:del s "k")` |
| `kv:keys` | `(kv:keys s)` · `(kv:keys s "prefix")` |

Params: positional `?` (pass an array) or named `:foo` / `@foo` (pass an object).
Column ↔ z mapping: NULL/INTEGER/REAL/TEXT/BLOB ↔ null/number/number/string/bytes.

## HTML / XML query

| Command | Example |
| --- | --- |
| `html:query` | `(html:query "li.a" html)` → array of outer-HTML strings |
| `html:text` | `(html:text "<b>hi</b>")` → `"hi"` |
| `html:attr` | `(html:attr "href" "<a href=\"x\">…</a>")` → `"x"` |
| `xml:query` | `(xml:query "/root/user/name" xml)` |
| `xml:text` | `(xml:text "<n>Ada</n>")` → `"Ada"` |
| `xml:attr` | `(xml:attr "id" "<x id=\"5\"/>")` → `"5"` |

Selector subset: `tag` · `.class` · `#id` · `[attr]` · `[attr=v]` · `[attr*=v]` · `[attr^=v]` · `[attr$=v]` · `a b` (descendant) · `a > b` (direct child).

## CLI flags

| Flag | Example | What it does |
| --- | --- | --- |
| `--version` / `-v` | `z --version` | Print version (e.g. `z 0.0.4`) |
| _(no args)_ | `z` | Start REPL (`z` plain) or syntax-coloured REPL (`zide`) |
| _(file path)_ | `z program.z` | Execute the script |
