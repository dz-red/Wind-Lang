const assert = require('assert').strict;
const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { readDiagnostics, rangeFor, findCompiler, checkCommand } = require('../diagnostics');

const compiler = path.resolve(process.argv[2] || './wind');
const workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'wind editor '));
const project = path.join(workspace, 'project with spaces');
const file = path.join(project, 'main.wnd');
const moduleFile = path.join(project, 'price.wnd');
const compilerName = process.platform === 'win32' ? 'wind.exe' : 'wind';

function check(text, status) {
    const [command, args] = checkCommand(compiler, file, false, project);
    const result = spawnSync(command, args, { cwd: project, input: text, encoding: 'utf8', timeout: 5000 });
    assert.ifError(result.error);
    assert.equal(result.status, status, result.stderr);
    assert.equal(result.stdout, '');
    const entries = readDiagnostics(result.stderr);
    assert.equal(entries.length, status);
    if (status) assert.doesNotThrow(() => JSON.parse(result.stderr));
    return entries[0];
}

try {
    fs.mkdirSync(project);
    fs.writeFileSync(file, 'terminal.past -> "old contents"\n');
    fs.writeFileSync(moduleFile, 'func price() -> int\n    return "100"\nend\n');

    const imported = check('use price\n', 1);
    assert.equal(imported.code, 'W2001');
    assert.equal(imported.file, moduleFile);
    const lines = fs.readFileSync(moduleFile, 'utf8').split('\n');
    assert.deepEqual(rangeFor(lines, imported.range), [1, 11, 1, 16]);
    assert.deepEqual(rangeFor(lines, imported.related.range), [0, 16, 0, 19]);

    const interp = ['str.title = "Wind"', 'terminal.paste -> "Привет 🌿 ${st.title}"'];
    const diagnostic = check(interp.join('\n'), 1);
    assert.equal(diagnostic.code, 'W2003');
    assert.equal(diagnostic.file, file);
    const start = interp[1].indexOf('st.title');
    assert.deepEqual(rangeFor(interp, diagnostic.range), [1, start, 1, start + 8]);
    check('terminal.paste -> "new contents"\n', 0);
    assert.equal(fs.readFileSync(file, 'utf8'), 'terminal.past -> "old contents"\n');
    assert.equal(fs.existsSync(path.join(project, 'output_ast.c')), false);

    fs.writeFileSync(path.join(workspace, compilerName), '');
    assert.equal(findCompiler(file, workspace, ''), path.join(workspace, compilerName));
    assert.equal(findCompiler(file, project, ''), compilerName);
    assert.equal(findCompiler(file, workspace, './custom compiler'), path.join(workspace, 'custom compiler'));
    assert.deepEqual(checkCommand(compiler, file, true, project), [
        'flatpak-spawn',
        ['--host', '--watch-bus', `--directory=${project}`, compiler, '--check', '--stdin', '--diag-json', file]
    ]);
    console.log('Wind VS Code diagnostics: OK');
} finally {
    fs.unlinkSync(file);
    fs.unlinkSync(moduleFile);
    if (fs.existsSync(path.join(workspace, compilerName))) fs.unlinkSync(path.join(workspace, compilerName));
    fs.rmdirSync(project);
    fs.rmdirSync(workspace);
}
