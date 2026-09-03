/*
 * Wind Language — VS Code extension.
 * Realtime diagnostics: при изменении .wnd буфера запускаем `./wind --check`
 * и парсим вывод в squiggly-подчёркивания (Problems panel + inline).
 *
 * Зависимости: только встроенные API (vscode, child_process, fs, path, os).
 * Никакого npm install не нужно.
 */

const vscode = require('vscode');
const { exec } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

let diagCollection;
let debounceTimers = new Map(); // doc URI → setTimeout handle
const DEBOUNCE_MS = 400;

/* Запущен ли VS Code в flatpak-sandbox. Тогда wind на хосте, нужен flatpak-spawn. */
const IN_FLATPAK = fs.existsSync('/.flatpak-info');

function activate(context) {
    diagCollection = vscode.languages.createDiagnosticCollection('wind');
    context.subscriptions.push(diagCollection);

    /* При изменении — debounced check. */
    context.subscriptions.push(
        vscode.workspace.onDidChangeTextDocument(e => {
            if (e.document.languageId !== 'wind') return;
            scheduleCheck(e.document);
        })
    );

    /* При открытии — сразу check. */
    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(doc => {
            if (doc.languageId === 'wind') checkDocument(doc);
        })
    );

    /* При сохранении — сразу check (без debounce). */
    context.subscriptions.push(
        vscode.workspace.onDidSaveTextDocument(doc => {
            if (doc.languageId === 'wind') checkDocument(doc);
        })
    );

    /* При закрытии — очистка diagnostics. */
    context.subscriptions.push(
        vscode.workspace.onDidCloseTextDocument(doc => {
            diagCollection.delete(doc.uri);
            const t = debounceTimers.get(doc.uri.toString());
            if (t) { clearTimeout(t); debounceTimers.delete(doc.uri.toString()); }
        })
    );

    /* Initial check для всех уже открытых .wnd. */
    vscode.workspace.textDocuments.forEach(doc => {
        if (doc.languageId === 'wind') checkDocument(doc);
    });
}

function scheduleCheck(doc) {
    const key = doc.uri.toString();
    const existing = debounceTimers.get(key);
    if (existing) clearTimeout(existing);
    const t = setTimeout(() => {
        debounceTimers.delete(key);
        checkDocument(doc);
    }, DEBOUNCE_MS);
    debounceTimers.set(key, t);
}

function checkDocument(doc) {
    const origPath = doc.uri.fsPath;
    if (!origPath) return;

    const projectDir = findProjectDir(origPath);
    if (!projectDir) {
        diagCollection.set(doc.uri, []);
        return;
    }
    const windBin = path.join(projectDir, 'wind');

    /* Пишем текущий буфер во временный файл — он может отличаться от диска. */
    const tmpFile = path.join(os.tmpdir(), `wind_check_${process.pid}_${Date.now()}.wnd`);
    try {
        fs.writeFileSync(tmpFile, doc.getText());
    } catch (e) {
        return;
    }

    /* В sandbox запускаем wind через flatpak-spawn --host. */
    const cmd = IN_FLATPAK
        ? `flatpak-spawn --host "${windBin}" --check --errors-simple "${tmpFile}"`
        : `"${windBin}" --check --errors-simple "${tmpFile}"`;

    exec(cmd, { cwd: projectDir, timeout: 5000 }, (err, stdout, stderr) => {
        try { fs.unlinkSync(tmpFile); } catch (e) {}
        const output = (stdout || '') + (stderr || '');
        const diagnostics = parseDiagnostics(output, doc, tmpFile);
        diagCollection.set(doc.uri, diagnostics);
    });
}

/* Ищем wind бинарник в той же папке или в одной из 5 parent. */
function findProjectDir(filePath) {
    let dir = path.dirname(filePath);
    for (let i = 0; i < 5; i++) {
        if (fs.existsSync(path.join(dir, 'wind'))) return dir;
        const parent = path.dirname(dir);
        if (parent === dir) break;
        dir = parent;
    }
    return null;
}

function parseDiagnostics(output, doc, tmpFile) {
    /* Формат: file:line:col: error|note: message */
    const re = /^(.+?):(\d+):(\d+):\s*(error|note|warning):\s*(.+)$/gm;
    const lastError = new Map(); // line → { msg, range } для прикрепления note
    const diagnostics = [];
    let m;
    while ((m = re.exec(output)) !== null) {
        const [, , lineStr, colStr, severity, message] = m;
        const lineNum = parseInt(lineStr, 10) - 1;
        const colNum = parseInt(colStr, 10) - 1;
        if (lineNum < 0 || lineNum >= doc.lineCount) continue;
        const lineText = doc.lineAt(lineNum).text;
        const range = new vscode.Range(
            lineNum, Math.min(colNum, lineText.length),
            lineNum, lineText.length
        );
        if (severity === 'note') {
            /* note приклеиваем к последней error на той же строке. */
            const prev = lastError.get(lineNum);
            if (prev) {
                prev.diag.message = prev.diag.message + ' (' + message + ')';
                continue;
            }
            /* note без предыдущей error — показываем как Information. */
            diagnostics.push(new vscode.Diagnostic(range, message,
                vscode.DiagnosticSeverity.Information));
            continue;
        }
        const sev = severity === 'warning'
            ? vscode.DiagnosticSeverity.Warning
            : vscode.DiagnosticSeverity.Error;
        const diag = new vscode.Diagnostic(range, message, sev);
        diag.source = 'wind';
        diagnostics.push(diag);
        lastError.set(lineNum, { diag });
    }
    return diagnostics;
}

function deactivate() {
    if (diagCollection) {
        diagCollection.clear();
        diagCollection.dispose();
    }
    for (const t of debounceTimers.values()) clearTimeout(t);
    debounceTimers.clear();
}

module.exports = { activate, deactivate };
