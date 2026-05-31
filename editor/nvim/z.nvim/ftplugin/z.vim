" Buffer-local settings for the z language.
" Loaded automatically when 'filetype' becomes 'z'.

if exists("b:did_ftplugin") | finish | endif
let b:did_ftplugin = 1

" `;` starts a line comment; `; %s` is the commentstring most plugins expect.
setlocal commentstring=;\ %s
setlocal comments=:;

" Lisp-style indentation. Neovim's built-in lisp indenter handles s-expressions
" well enough for a language that has no significant whitespace.
setlocal lisp
setlocal autoindent
setlocal lispwords=if,when,unless,cond,let,do,while,for,fn,lambda,set,try,catch,and,or,quote

" Treat hyphens, colons, and ? as part of identifiers (e.g. `starts-with`,
" `img:create`, predicates ending in `?`).
setlocal iskeyword+=-,:,?,!,*

" Make `%` jump between parentheses and brackets cleanly.
setlocal matchpairs+=<:>

" 2-space indent is conventional for s-expressions.
setlocal expandtab
setlocal shiftwidth=2
setlocal softtabstop=2

let b:undo_ftplugin = "setlocal commentstring< comments< lisp< autoindent<"
      \ . " lispwords< iskeyword< matchpairs< expandtab< shiftwidth< softtabstop<"
