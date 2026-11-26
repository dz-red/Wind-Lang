const assert = require('assert').strict;
const vscode = require('vscode');

function waitFor(uri, predicate) {
    return new Promise((resolve, reject) => {
        const dispose = () => { subscription.dispose(); clearTimeout(timer); };
        const check = () => {
            const diagnostics = vscode.languages.getDiagnostics(uri);
            if (!predicate(diagnostics)) return;
            dispose();
            resolve(diagnostics);
        };
        const subscription = vscode.languages.onDidChangeDiagnostics(check);
        const timer = setTimeout(() => {
            dispose();
            reject(new Error(`Timed out waiting for diagnostics on ${uri.fsPath}`));
        }, 7000);
        check();
    });
}

async function run() {
    assert.equal(vscode.workspace.isTrusted, true);
    const root = vscode.workspace.workspaceFolders[0].uri;
    const main = vscode.Uri.joinPath(root, 'editor-test.wnd');
    const imported = vscode.Uri.joinPath(root, 'editor_module.wnd');
    const onDisk = 'use editor_module\n';
    await vscode.workspace.fs.writeFile(main, Buffer.from(onDisk));
    await vscode.workspace.fs.writeFile(imported, Buffer.from('func price() -> int\n    return "100"\nend\n'));
    const doc = await vscode.workspace.openTextDocument(main);
    const extension = vscode.extensions.getExtension('DZ-RED.wind-language');
    await extension.activate();

    const [error] = await waitFor(imported, items => items.some(item => item.code === 'W2001'));
    assert.equal(error.relatedInformation[0].location.range.start.character, 16);
    assert.equal(vscode.languages.getDiagnostics(main).length, 0);

    const buffer = 'str.title = "Wind"\nterminal.paste -> "Привет 🌿 ${st.title}"';
    const replace = async text => {
        const edit = new vscode.WorkspaceEdit();
        edit.replace(main, new vscode.Range(doc.positionAt(0), doc.positionAt(doc.getText().length)), text);
        assert.equal(await vscode.workspace.applyEdit(edit), true);
    };
    await replace(buffer);
    const [interpolation] = await waitFor(main, items => items.some(item => item.code === 'W2003'));
    assert.equal(doc.getText(interpolation.range), 'st.title');
    assert.deepEqual(vscode.languages.getDiagnostics(imported), []);
    assert.equal(Buffer.from(await vscode.workspace.fs.readFile(main)).toString(), onDisk);

    await replace('terminal.paste -> "Wind"\n');
    await waitFor(main, items => items.length === 0);
}

module.exports = { run };
