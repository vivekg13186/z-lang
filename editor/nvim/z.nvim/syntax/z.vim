" Vim syntax file for the z language.
" Maintainer: z-lang
"
" Highlights: line comments, strings (with ${...} interpolation),
" numbers, booleans/null, special forms, built-in functions, and
" function definitions.

if exists("b:current_syntax") | finish | endif

syntax case match
syntax iskeyword @,48-57,-,_,!,?,*,:,/

" -------- Comments --------
" `;` to end of line. Three-semicolon comments are sometimes used as section
" headers in Lisps; flag them as Todo for visibility.
syntax match zComment       /;.*$/ contains=zCommentTodo
syntax match zCommentTodo   /\v;{2,}.*$/ contained
syntax keyword zTodo contained TODO FIXME XXX NOTE

" -------- Numbers --------
syntax match zNumber        /\v<-?\d+(\.\d+)?([eE][-+]?\d+)?>/
syntax match zNumber        /\v<-?0x[0-9a-fA-F]+>/

" -------- Booleans / null --------
syntax keyword zBoolean     true false
syntax keyword zConstant    null nil pi e

" -------- Strings (with ${...} interpolation) --------
syntax region zString       start=+"+ skip=+\\.+ end=+"+ contains=zStringEscape,zInterpolation
syntax match  zStringEscape /\\./ contained
syntax region zInterpolation matchgroup=zInterpolationDelim
      \ start=+\${+ end=+}+ contained contains=TOP

" -------- Special forms (head position only is hard without a parser;
"           highlight everywhere — these names are unlikely to be rebound). --------
syntax keyword zSpecialForm do if when unless cond let
                           \ while for fn lambda set try catch
                           \ quote and or else
syntax match   zSpecialForm /\v(\(\s*)@<=(-\>\>?|\&)(\s|\))@=/
syntax match   zSpecialForm /\v(\&\&|\|\|)/

" -------- Built-in functions --------
" Core / I/O / control
" NOTE: `contains` is a z builtin but it collides with the Vim syntax option
" name of the same word; matched separately below to avoid E395.
syntax keyword zBuiltin print read read-lines write append delete
                       \ input scanf exec exit run import help argv env
                       \ list-dir file-info copy-file move-file
                       \ assert sleep type now timestamp format-date
                       \ length get put push pop keys values entries
                       \ array object index-of
syntax match zBuiltin /\<contains\>/

" Strings
syntax keyword zBuiltin concat split join trim lower upper replace
                       \ substring between levenshtein reverse
                       \ starts-with ends-with

" Higher-order
syntax keyword zBuiltin map filter reduce sort chunk

" Math
syntax keyword zBuiltin min max floor ceil round trunc abs sign mod
                       \ sqrt cbrt pow exp log log2 log10
                       \ sin cos tan asin acos atan atan2
                       \ sinh cosh tanh random

" Regex
syntax keyword zBuiltin regex:test regex:match regex:find-all
                       \ regex:replace regex:split

" Encoding / crypto / hash
syntax keyword zBuiltin base64:encode base64:decode encrypt decrypt
                       \ uuid md5 sha256 sha512

" URL / JSON
syntax keyword zBuiltin url:encode url:decode url:build
                       \ json:parse json:stringify

" HTTP
syntax keyword zBuiltin http:get http:post

" Archive
syntax keyword zBuiltin zip:create zip:extract tar:create tar:extract

" Image (optional module)
syntax keyword zBuiltin img:create img:resize img:crop img:rotate
                       \ img:compose img:replace-color img:circle img:rect
                       \ img:add-text img:bw img:grayscale img:to-pdf
                       \ img:qr img:barcode img:info

" -------- Operators (when they appear as the head of a call) --------
" The character class already covers ==, !=, <=, >= as multi-char strings.
syntax match zOperator /\v(\(\s*)@<=[-+*/%<>!=]+/

" -------- Function-definition highlight --------
"   (fn name (args) body) — pull out the name token after `(fn `.
syntax match zFuncName /\v(\(\s*fn\s+)@<=\S+/

" -------- Parens / brackets --------
syntax match zDelimiter /[()\[\]]/

" -------- Linking --------
hi def link zComment           Comment
hi def link zCommentTodo       Todo
hi def link zTodo              Todo
hi def link zNumber            Number
hi def link zBoolean           Boolean
hi def link zConstant          Constant
hi def link zString            String
hi def link zStringEscape      SpecialChar
hi def link zInterpolation     Identifier
hi def link zInterpolationDelim PreProc
hi def link zSpecialForm       Statement
hi def link zBuiltin           Function
hi def link zOperator          Operator
hi def link zFuncName          Function
hi def link zDelimiter         Delimiter

let b:current_syntax = "z"
