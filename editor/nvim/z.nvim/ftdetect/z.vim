" Fallback for plain Vim, which doesn't load ftdetect/*.lua.
" Neovim uses ftdetect/z.lua in preference.
au BufRead,BufNewFile *.z setfiletype z
