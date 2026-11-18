// The Musil extension: Cmd-Enter sends the block around the cursor (or the selection) to the
// evaluation port of a running Listener or `musil --serve 7770 -i`; Cmd-Shift-Enter the file;
// Cmd-Alt-Enter the line. The reply (a value, "ok", or an error) shows in the status bar.
const vscode = require('vscode');
const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');

// the port's token: the first line of every message (musil writes it when the port opens, readable by the
// user alone: ~/.musil/port-<port>.token, else <tmp>/musil-<uid>/port-<port>.token)
function token(port) {
    const name = 'port-' + port + '.token';
    const places = [path.join(os.homedir(), '.musil', name)];
    if (process.getuid) places.push(path.join(os.tmpdir(), 'musil-' + process.getuid(), name));
    for (const f of places) {
        try { const t = fs.readFileSync(f, 'utf8').split('\n')[0].trim(); if (t) return t; } catch (e) { }
    }
    return '';
}

function send(text) {
    const cfg = vscode.workspace.getConfiguration('musil');
    const tok = token(cfg.get('port'));
    if (!tok) return Promise.resolve('no token for port ' + cfg.get('port') + ': is musil --serve running on this machine?');
    return new Promise((resolve) => {
        const sock = new net.Socket(); let reply = '';
        sock.setTimeout(3000);
        sock.on('data', (d) => { reply += d.toString(); });
        sock.on('close', () => resolve(reply.trim() || 'no reply'));
        sock.on('error', (e) => resolve('cannot reach the Listener (' + e.message + '): is it running, is the port ' + cfg.get('port') + '?'));
        sock.on('timeout', () => { sock.destroy(); resolve('no reply (timeout)'); });
        sock.connect(cfg.get('port'), cfg.get('host'), () => { sock.end(tok + '\n' + text + '\n\x04'); });
    });
}
// the block around the cursor: the lines between blank lines, extended until brackets balance
function blockAt(doc, line) {
    let a = line, b = line;
    while (a > 0 && doc.lineAt(a - 1).text.trim() !== '') a--;
    while (b + 1 < doc.lineCount && doc.lineAt(b + 1).text.trim() !== '') b++;
    let text = ''; for (let k = a; k <= b; k++) text += doc.lineAt(k).text + '\n';
    return { text, a, b };
}
function flash(editor, a, b) {
    const deco = vscode.window.createTextEditorDecorationType({ backgroundColor: 'rgba(90, 140, 220, 0.25)', isWholeLine: true });
    editor.setDecorations(deco, [new vscode.Range(a, 0, b, 0)]);
    setTimeout(() => deco.dispose(), 350);
}
async function run(editor, text, a, b) {
    flash(editor, a, b);
    const reply = await send(text);
    vscode.window.setStatusBarMessage('musil: ' + reply.split('\n').pop(), 4000);
    if (reply.startsWith('error:')) vscode.window.showWarningMessage(reply);
}
function activate(context) {
    context.subscriptions.push(
        vscode.commands.registerCommand('musil.sendBlock', () => {
            const ed = vscode.window.activeTextEditor; if (!ed) return;
            if (!ed.selection.isEmpty) { run(ed, ed.document.getText(ed.selection), ed.selection.start.line, ed.selection.end.line); return; }
            const blk = blockAt(ed.document, ed.selection.active.line); run(ed, blk.text, blk.a, blk.b);
        }),
        vscode.commands.registerCommand('musil.sendFile', () => {
            const ed = vscode.window.activeTextEditor; if (!ed) return;
            run(ed, ed.document.getText(), 0, ed.document.lineCount - 1);
        }),
        vscode.commands.registerCommand('musil.sendLine', () => {
            const ed = vscode.window.activeTextEditor; if (!ed) return;
            const l = ed.selection.active.line; run(ed, ed.document.lineAt(l).text + '\n', l, l);
        }));
}
function deactivate() {}
module.exports = { activate, deactivate };
