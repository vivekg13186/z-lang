// Activation entry point. Registers two commands that run the active .z file
// through the configured `z` / `zide` binary in an integrated terminal.
//
// No external dependencies — VS Code exposes the `vscode` module at runtime.

const vscode = require("vscode");

/** Shared terminal so successive runs reuse the same panel. */
let runTerminal = null;

function getTerminal() {
  if (!runTerminal || runTerminal.exitStatus) {
    runTerminal = vscode.window.createTerminal("z");
  }
  return runTerminal;
}

function runActive(binary) {
  const editor = vscode.window.activeTextEditor;
  if (!editor) {
    vscode.window.showWarningMessage("z: no active editor.");
    return;
  }
  const doc = editor.document;
  if (doc.languageId !== "z") {
    vscode.window.showWarningMessage("z: active file is not a .z file.");
    return;
  }
  // Save first; the binary reads from disk.
  doc.save().then(() => {
    const term = getTerminal();
    term.show(true);
    // Quote the path so spaces survive any shell.
    term.sendText(`${binary} "${doc.fileName}"`);
  });
}

function activate(context) {
  const cfg = () => vscode.workspace.getConfiguration("z");

  context.subscriptions.push(
    vscode.commands.registerCommand("z.run", () => {
      runActive(cfg().get("binaryPath", "z"));
    }),
    vscode.commands.registerCommand("z.runZide", () => {
      runActive(cfg().get("zideBinaryPath", "zide"));
    }),
    vscode.window.onDidCloseTerminal((t) => {
      if (t === runTerminal) runTerminal = null;
    })
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
