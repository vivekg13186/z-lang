# z Language Cheat Sheet

Every command with a one-line example. Run any of these in the REPL (`./z` or
`./zide`) or in a `.z` file.

## Special forms

| Command | Example |
| --- | --- |
| `do` | `(do (print "a") (print "b"))` |
| `if` | `(if (> x 0) "pos" "neg")` |
| `while` | `(while (< i 10) (set i (+ i 1)))` |
| `for` | `(for x (array 1 2 3) (print x))` |
| `fn` | `(fn add (a b) (+ a b))` |
| `lambda` | `(lambda (x) (* x 2))` |
| `set` | `(set name "vivek")` |
| `try` / `catch` | `(try (/ 1 0) (catch e (print e)))` |
| `quote` | `(quote (a b c))` |
| `and` | `(and true false)` |
| `or` | `(or false true)` |

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
| `map` | `(map (lambda (x) (* x x)) (array 1 2 3))` |
| `filter` | `(filter (lambda (x) (> x 2)) (array 1 2 3))` |
| `reduce` | `(reduce (lambda (a b) (+ a b)) (array 1 2 3) 0)` |

## Strings

| Command | Example |
| --- | --- |
| `concat` | `(concat "foo" "bar")` |
| `split` | `(split "," "a,b,c")` |
| `trim` | `(trim "  hi  ")` |
| `lower` | `(lower "ABC")` |
| `upper` | `(upper "abc")` |
| `replace` | `(replace "hello" "l" "L")` |
| `substring` | `(substring "hello" 0 3)` |
| `starts-with` | `(starts-with "hello" "he")` |
| `ends-with` | `(ends-with "hello" "lo")` |
| `contains` | `(contains "hello" "ell")` |
| `index-of` | `(index-of "hello" "l")` |
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
| `random` | `(random)` |

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
| `img:circle` | `(img:circle "c.png" "c.png" 100 100 50 "red")` |
| `img:rect` | `(img:rect "c.png" "c.png" 10 10 80 40 "blue")` |
| `img:add-text` | `(img:add-text "in.png" "out.png" "hi" 10 30 24 "white")` |
| `img:bw` | `(img:bw "in.png" "out.png")` |
| `img:grayscale` | `(img:grayscale "in.png" "out.png")` |
| `img:to-pdf` | `(img:to-pdf (array "a.png" "b.png") "out.pdf")` |
| `img:qr` | `(img:qr "https://example.com" "qr.png" 6)` |
| `img:barcode` | `(img:barcode "012345678905" "code.png" "ean13")` |
