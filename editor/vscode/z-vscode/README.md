# z language for VS Code

Syntax highlighting, snippets, and run support for the [z](https://github.com/vivekg13186/z-lang) language.

## Features

- File extension `.z` registered as the `z` language
- Syntax highlighting for comments, strings (with `${...}` interpolation), numbers, special forms (`if`, `fn`, `lambda`, `let`, `do`, `while`, `for`, `try`, `and`/`or`, ...), and every built-in function (`print`, `map`, `regex:*`, `img:*`, `sha256`, ...)
- Function definitions get an `entity.name.function` highlight on the name in `(fn name ...)`
- `;` line-comment toggling with the standard VS Code keybinding
- Auto-close for `(`, `[`, `"`
- Snippets for `fn`, `lambda`, `if`, `while`, `for`, `try`, `let`, `set`, `print`, `map`, `filter`, `reduce`
- `z: Run current file` (default keybinding `Ctrl+F5` / `Cmd+F5`)
- `z: Run current file in zide` for the enhanced REPL

## Install

### From a packaged .vsix (recommended)

```sh
cd editor/vscode/z-vscode
npx --yes @vscode/vsce package          # produces z-language-0.0.4.vsix
code --install-extension z-language-0.0.4.vsix
```

### From source (no packaging)

Symlink or copy this folder into your VS Code extensions directory:

| OS      | Path                                  |
| ------- | ------------------------------------- |
| macOS / Linux | `~/.vscode/extensions/z-vscode` |
| Windows | `%USERPROFILE%\.vscode\extensions\z-vscode` |

Then reload VS Code (`Developer: Reload Window`).

## Configuration

| Setting | Default | Description |
| ------- | ------- | ----------- |
| `z.binaryPath`     | `z`    | Path to the `z` interpreter binary. |
| `z.zideBinaryPath` | `zide` | Path to the `zide` REPL binary. |

If `z` and `zide` aren't on your `$PATH`, set absolute paths in your User or Workspace settings:

```json
"z.binaryPath": "/usr/local/bin/z",
"z.zideBinaryPath": "/usr/local/bin/zide"
```

## Commands

| Command | Default keybinding | Action |
| ------- | ------------------ | ------ |
| `z.run`     | `Ctrl+F5` / `Cmd+F5` | Save and run the current file through `z` |
| `z.runZide` | — | Same, but with `zide` |

Both commands reuse a single integrated terminal named `z`.
