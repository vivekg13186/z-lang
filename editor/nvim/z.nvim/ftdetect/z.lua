-- Detect .z files as the `z` filetype. Kept tiny on purpose; richer setup
-- lives in ftplugin/z.vim so it loads only after the filetype fires.
vim.filetype.add({
  extension = {
    z = "z",
  },
})
