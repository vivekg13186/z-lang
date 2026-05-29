# z.nvim

Neovim plugin for the [z](https://github.com/vivekg13186/z-lang) language.

Provides:

- Filetype detection for `.z`
- Syntax highlighting (comments, strings with `${...}` interpolation, numbers, special forms, builtins, function names)
- Lisp-style indentation and a `;` commentstring
- `:Z` and `:Zide` to run the current buffer

## Install

### lazy.nvim

```lua
{
  "vivekg13186/z-lang",
  -- The plugin lives inside the language repo. If you split it out, use the
  -- standalone repo path instead.
  config = function()
    vim.opt.rtp:append(vim.fn.stdpath("data") .. "/lazy/z-lang/editor/nvim/z.nvim")
  end,
}
```

Or, more cleanly, after vendoring just the plugin folder:

```lua
{ dir = "~/path/to/z-lang/editor/nvim/z.nvim", name = "z.nvim", ft = "z" }
```

### packer.nvim

```lua
use {
  "vivekg13186/z-lang",
  rtp = "editor/nvim/z.nvim",
}
```

### Manual

Symlink or copy `editor/nvim/z.nvim` into your runtime path:

```sh
ln -s "$PWD/editor/nvim/z.nvim" ~/.local/share/nvim/site/pack/local/start/z.nvim
```

## Usage

Open a `.z` file — highlighting and indentation apply automatically. Then:

| Command | What it does |
| --- | --- |
| `:Z`    | Run the current buffer with `z`    in a terminal split |
| `:Zide` | Run the current buffer with `zide` in a terminal split |

Override the binary paths if `z` / `zide` aren't on `$PATH`:

```lua
vim.g.z_cmd    = "/usr/local/bin/z"
vim.g.zide_cmd = "/usr/local/bin/zide"
```

## Customizing highlights

The plugin links to standard groups (`Comment`, `String`, `Function`,
`Statement`, `Number`, ...), so the colors come from your colorscheme. To
tweak something specifically, override after `:set ft=z`:

```vim
highlight link zBuiltin Special
highlight link zFuncName Title
```
