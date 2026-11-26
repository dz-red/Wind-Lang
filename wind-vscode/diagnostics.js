const fs = require('fs');
const path = require('path');

function readDiagnostics(stderr) {
    const entries = [];
    for (const line of stderr.split(/\r?\n/)) {
        if (!line.startsWith('{')) continue;
        const item = JSON.parse(line);
        if (item.severity === 'error' && /^W\d{4}$/.test(item.code) &&
            typeof item.file === 'string' && typeof item.message === 'string') {
            entries.push(item);
        }
    }
    return entries;
}

function utf16Column(line, column) {
    const offset = Math.max(0, column - 1);
    let bytes = 0, units = 0;
    for (const char of line) {
        bytes += Buffer.byteLength(char);
        if (bytes > offset) break;
        units += char.length;
    }
    return units;
}

function rangeFor(lines, range) {
    if (!range) return [0, 0, 0, 0];
    const { line, column, endLine, endColumn } = range;
    if (!Number.isInteger(line) || !Number.isInteger(column) || line < 1 || line > lines.length) return null;
    const start = utf16Column(lines[line - 1], column);
    if (Number.isInteger(endLine) && Number.isInteger(endColumn) && endLine >= line && endLine <= lines.length) {
        const end = utf16Column(lines[endLine - 1], endColumn);
        if (endLine > line || end > start) return [line - 1, start, endLine - 1, end];
    }
    const char = String.fromCodePoint(lines[line - 1].codePointAt(start) ?? 0);
    return [line - 1, start, line - 1, Math.min(start + char.length, lines[line - 1].length)];
}

function findCompiler(file, workspace, configured) {
    const name = process.platform === 'win32' ? 'wind.exe' : 'wind';
    if (configured) {
        if (path.isAbsolute(configured)) return configured;
        return /[/\\]/.test(configured) ? path.resolve(workspace || path.dirname(file), configured) : configured;
    }
    if (workspace) {
        const root = path.resolve(workspace);
        for (let dir = path.dirname(path.resolve(file)); ; dir = path.dirname(dir)) {
            const relative = path.relative(root, dir);
            if (relative === '..' || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) break;
            const candidate = path.join(dir, name);
            try {
                if (fs.statSync(candidate).isFile()) return candidate;
            } catch {}
            if (dir === root) break;
        }
    }
    return name;
}

function checkCommand(compiler, file, flatpak, cwd) {
    const args = ['--check', '--stdin', '--diag-json', file];
    if (flatpak) return ['flatpak-spawn', ['--host', '--watch-bus', `--directory=${cwd}`, compiler, ...args]];
    return [compiler, args];
}

module.exports = { readDiagnostics, rangeFor, findCompiler, checkCommand };
