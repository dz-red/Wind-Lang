#!/bin/sh
set -eu

wind=$(realpath "${1:-./wind}")
tests=$(dirname "$(realpath "$0")")
tmp=$(mktemp -d)
trap 'rm -f "$tmp"/*.wnd "$tmp/error" "$tmp/expected" "$tmp/default" "$tmp/explain"; rmdir "$tmp"' EXIT
cd "$tmp"

reject() {
    status=0
    "$wind" "$@" > /dev/null 2> error || status=$?
    test "$status" = 1
}

printf '  terminal.past -> "Hello"\nterminal.paest -> "Bye"\n' > terminal.wnd
reject --diag=compact terminal.wnd
cat > expected <<'EOF'
terminal.wnd:1:12: error[W1001]: unknown terminal method `past`
terminal.wnd:2:10: error[W1001]: unknown terminal method `paest`
[wind] compilation failed: 2 errors
EOF
cmp expected error
reject terminal.wnd
cp error default
reject --diag=normal terminal.wnd
cmp default error
grep -Fx 'error[W1001]: unknown terminal method `past`' error
grep -Fx '  --> terminal.wnd:1:12' error
grep -Fx '  │            ^^^^' error
if grep -q 'SUGGESTIONS' error; then exit 1; fi
reject --diag=learn terminal.wnd
grep -Fx 'EXPLANATION' error
grep -Fx '  Use `terminal.paste` to display a value:' error
grep -Fx '    terminal.paste -> "Hello"' error
grep -Fx '  wind --explain W1001' error

cat > returns.wnd <<'EOF'
func price() -> int
    return "100"
end
func label() -> str
    return 42 + 1
end
EOF
reject --diag=compact returns.wnd
cat > expected <<'EOF'
returns.wnd:2:12: error[W2001]: expected `int`, found `str` in function `price`
returns.wnd:5:12: error[W2001]: expected `str`, found `int` in function `label`
[wind] compilation failed: 2 errors
EOF
cmp expected error
reject --boehm --diag=normal --check returns.wnd
grep -Fx 'error[W2001]: return type mismatch' error
grep -Fx '  │                 ─── expected `int`' error
grep -Fx '  │            ^^^^^ found `str`' error
grep -Fx '  │            ^^^^^^ found `int`' error
reject --diag=learn --check returns.wnd
grep -F 'return int(text)' error
grep -Fx '  wind --explain W2001' error
"$wind" --explain W2001 > explain
grep -Fx '  Example:' explain
grep -F 'return 100' explain

printf 'use returns\n' > main.wnd
reject --diag=compact --check main.wnd
cmp expected error
reject --errors-simple --check main.wnd
grep -F 'returns.wnd:2:12: error: expected `int`, found `str`' error

printf 'terminal.paste -> "unclosed' > lex.wnd
reject --diag=compact lex.wnd
grep -Fx 'lex.wnd:1:19: error[W1000]: unterminated string' error
printf 'use absent\n' > missing.wnd
reject --diag=compact missing.wnd
grep -F 'missing.wnd:1:5: error[W3001]: cannot read module' error
reject --diag=invalid lex.wnd
grep -F 'unknown diagnostic mode' error

cat > interp.wnd <<'EOF'
str.title = "Wind"
terminal.paste -> "=== ${st.title} ==="
EOF
reject --diag=learn interp.wnd
cp error default
reject --diag=learn -s interp.wnd
cmp default error
reject -s --diag=learn interp.wnd
cmp default error
grep -Fx 'error[W2003]: unknown type prefix `st`' error
grep -Fx '  --> interp.wnd:2:26' error
grep -Fx '  │                          ^^^^^^^^' error
grep -Fx 'note: `title` is declared as `str`' error
grep -Fx '  Use `${str.title}` or `${title}`.' error
grep -Fx '  wind --explain W2003' error
reject --diag=compact -s interp.wnd
grep -Fx 'interp.wnd:2:26: error[W2003]: unknown type prefix `st` for `title`' error
"$wind" --explain W2003 > explain
grep -F '${title}' explain

cat > escaped.wnd <<'EOF'
str.title = "Wind"
terminal.paste -> "\"${title}\"\n${  st.title  }"
EOF
reject --diag=compact escaped.wnd
grep -Fx 'escaped.wnd:2:38: error[W2003]: unknown type prefix `st` for `title`' error

printf 'func answer() -> int\n    return int("42")\nend\n' > good.wnd
PATH=/nonexistent "$wind" --check good.wnd
test ! -e output_ast.c
test ! -e app
if command -v python3 > /dev/null; then
    python3 "$tests/diagnostic-colors.py" "$wind" returns.wnd
fi
printf '%s\n' 'Diagnostics: OK'
