# Wind Language

Syntax highlighting, file icons, and live compiler diagnostics for `.wnd` files.
The Problems panel shows Wind error codes, exact ranges, and related locations
such as the declared return type of a function.

## Setup

1. Build the current Wind compiler with `make`.
2. Open the project folder in VS Code and trust it if you intend to run its compiler.
3. If Wind is not in the project or on `PATH`, set **Wind: Compiler Path** in
   VS Code to the compiler executable.

Live checks use the unsaved editor text and resolve imports next to the actual
file. Imported modules are read from disk, so save a changed module before
checking files that import it. Checks do not run GCC or the program. Check
failures such as a missing or outdated compiler appear in the **Wind** output channel.

Highlighting also works in Restricted Mode, where the extension does not run
the compiler.

From a Wind checkout, run the extension integration tests with
`node wind-vscode/tests/diagnostics.test.js ./wind` after building Wind.
