# Z Language — VS Code Extension

Syntax highlighting, bracket matching, and snippets for **z**, the tiny
Lisp-flavoured workflow language defined in this repo.

## What it gives you

- Syntax highlighting for comments, strings, numbers, special forms, builtins, and operators
- Bracket matching and auto-closing for `()`, `[]`, `{}`, and `"`
- Comment toggling with `Ctrl+/` (or `Cmd+/` on macOS)
- A handful of snippets — type a prefix and hit `Tab`:

  | Prefix | Expands to |
  | ------ | ---------- |
  | `fn`     | named function |
  | `lambda` | anonymous function |
  | `if`     | if / else |
  | `while`  | while loop |
  | `for`    | for-each loop |
  | `do`     | sequential block |
  | `set`    | variable definition |
  | `try`    | try / catch |
  | `obj`    | object literal |
  | `arr`    | array literal |
  | `print`  | print |
  | `map`    | map over a collection |
  | `filter` | filter a collection |
  | `reduce` | reduce a collection |
  | `jparse` / `jstr` | JSON parse / stringify |
  | `run`    | shell command |
  | `main`   | top-level `(do ...)` scaffold |

## Install (development mode)

Two easy ways:

### A. Symlink the folder into your VS Code extensions directory

macOS / Linux:

```
ln -s "$(pwd)/plugin" ~/.vscode/extensions/z-lang-0.1.0
```

Windows (PowerShell, admin shell):

```
New-Item -ItemType SymbolicLink -Path "$env:USERPROFILE\.vscode\extensions\z-lang-0.1.0" -Target (Resolve-Path .\plugin)
```

Restart VS Code. Open any `.z` file and you should see highlighting.

### B. Package it as a `.vsix` and install

Install the packaging tool once:

```
npm install -g @vscode/vsce
```

Then from the `plugin/` folder:

```
cd plugin
vsce package
code --install-extension z-lang-0.1.0.vsix
```

## Files

```
plugin/
├── package.json                 — extension manifest
├── language-configuration.json  — brackets, comments, auto-close
├── syntaxes/
│   └── z.tmLanguage.json        — TextMate grammar
└── snippets/
    └── z.json                   — code snippets
```

This extension is purely declarative — no JavaScript runtime, no activation
events, no compile step. Edit the JSON files and reload VS Code (`Cmd/Ctrl
+ Shift + P` → "Developer: Reload Window") to see your changes.
