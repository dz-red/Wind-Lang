const vscode = require('vscode');
const { execFile } = require('child_process');
const fs = require('fs');
const path = require('path');
const { readDiagnostics, rangeFor, findCompiler, checkCommand } = require('./diagnostics');

const DEBOUNCE_MS = 400;
const IN_FLATPAK = fs.existsSync('/.flatpak-info');
const pending = new Map();
const results = new Map();
let collection, output;

function isWind(doc) {
    return doc.languageId === 'wind' && doc.uri.scheme === 'file';
}

function activate(context) {
    collection = vscode.languages.createDiagnosticCollection('wind');
    output = vscode.window.createOutputChannel('Wind');
    const refresh = () => vscode.workspace.textDocuments.filter(isWind).forEach(doc => schedule(doc));
    context.subscriptions.push(collection, output,
        vscode.workspace.onDidOpenTextDocument(doc => schedule(doc)),
        vscode.workspace.onDidChangeTextDocument(({ document, contentChanges }) => {
            if (contentChanges.length) schedule(document, DEBOUNCE_MS);
        }),
        vscode.workspace.onDidSaveTextDocument(doc => { if (isWind(doc)) refresh(); }),
        vscode.workspace.onDidCloseTextDocument(doc => {
            const key = doc.uri.toString();
            cancel(key);
            publish(key, new Map());
        }),
        vscode.workspace.onDidGrantWorkspaceTrust(refresh),
        vscode.workspace.onDidChangeConfiguration(event => {
            if (event.affectsConfiguration('wind.compilerPath')) refresh();
        })
    );
    refresh();
}

function cancel(key) {
    const job = pending.get(key);
    if (!job) return;
    pending.delete(key);
    clearTimeout(job.timer);
    if (job.child && job.child.exitCode === null) job.child.kill();
}

function schedule(doc, delay = 0) {
    if (!isWind(doc) || !vscode.workspace.isTrusted) return;
    const key = doc.uri.toString();
    cancel(key);
    const job = { version: doc.version };
    pending.set(key, job);
    job.timer = setTimeout(() => check(doc, key, job), delay);
}

function current(doc, key, job) {
    return pending.get(key) === job && !doc.isClosed && doc.version === job.version;
}

function check(doc, key, job) {
    if (!current(doc, key, job)) return;
    const file = doc.uri.fsPath;
    const workspace = vscode.workspace.getWorkspaceFolder(doc.uri);
    const configured = vscode.workspace.getConfiguration('wind', doc.uri).get('compilerPath', '');
    const compiler = findCompiler(file, workspace?.uri.fsPath, configured);
    const cwd = path.dirname(file);
    const [command, args] = checkCommand(compiler, file, IN_FLATPAK, cwd);
    const text = doc.getText();
    job.child = execFile(command, args, { cwd, timeout: 5000, maxBuffer: 1024 * 1024, windowsHide: true },
        async (error, stdout, stderr) => {
            if (!current(doc, key, job)) return;
            let groups = new Map();
            try {
                const entries = readDiagnostics(stderr);
                if (error && !entries.length) {
                    const reason = stderr.trim() || stdout.trim() || error.message;
                    output.appendLine(`Cannot check ${file}: ${reason}`);
                } else {
                    groups = await makeDiagnostics(entries, doc, text);
                }
            } catch (error) {
                output.appendLine(`Cannot read diagnostics for ${file}: ${error.message}`);
            }
            if (!current(doc, key, job)) return;
            pending.delete(key);
            publish(key, groups);
        });
    job.child.stdin.on('error', () => {});
    job.child.stdin.end(text, 'utf8');
}

async function makeDiagnostics(entries, doc, text) {
    const root = path.resolve(doc.uri.fsPath);
    const files = new Map();
    const groups = new Map();
    for (const entry of entries) {
        const file = path.resolve(path.dirname(root), entry.file);
        if (!files.has(file)) {
            const uri = file === root ? doc.uri : vscode.Uri.file(file);
            const open = vscode.workspace.textDocuments.find(item => item.uri.toString() === uri.toString());
            const source = file === root ? text :
                open?.isDirty ? null : await fs.promises.readFile(file, 'utf8').catch(() => null);
            files.set(file, { uri, lines: source?.split(/\r?\n/) });
        }
        const { uri, lines } = files.get(file);
        if (!lines) continue;
        const primary = rangeFor(lines, entry.range);
        if (!primary) continue;
        const message = entry.note ? `${entry.message}\n${entry.note}` : entry.message;
        const diagnostic = new vscode.Diagnostic(new vscode.Range(...primary), message, vscode.DiagnosticSeverity.Error);
        diagnostic.source = 'Wind';
        diagnostic.code = entry.code;
        const related = rangeFor(lines, entry.related?.range);
        if (entry.related && related) {
            diagnostic.relatedInformation = [new vscode.DiagnosticRelatedInformation(
                new vscode.Location(uri, new vscode.Range(...related)), entry.related.message
            )];
        }
        const key = uri.toString();
        if (!groups.has(key)) groups.set(key, { uri, diagnostics: [] });
        groups.get(key).diagnostics.push(diagnostic);
    }
    return groups;
}

function publish(root, groups) {
    const old = results.get(root) || new Map();
    if (groups.size) results.set(root, groups);
    else results.delete(root);
    const touched = new Map([...old, ...groups]);
    if (!touched.has(root)) touched.set(root, { uri: vscode.Uri.parse(root) });
    for (const [key, { uri }] of touched) {
        const unique = new Map();
        const dirty = vscode.workspace.textDocuments.some(doc => doc.isDirty && doc.uri.toString() === key);
        for (const [owner, result] of results) {
            if (dirty && key !== owner) continue;
            for (const diagnostic of result.get(key)?.diagnostics || []) {
                const { start, end } = diagnostic.range;
                const id = JSON.stringify([diagnostic.code, diagnostic.message,
                    start.line, start.character, end.line, end.character]);
                unique.set(id, diagnostic);
            }
        }
        if (unique.size) collection.set(uri, [...unique.values()]);
        else collection.delete(uri);
    }
}

function deactivate() {
    for (const key of pending.keys()) cancel(key);
    results.clear();
}

module.exports = { activate, deactivate };
