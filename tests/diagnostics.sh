#!/bin/sh
set -eu

wind=$(realpath "${1:-./wind}")
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
grep -Fx '  --> terminal.wnd:1:12' error
grep -Fx '  │            ^^^^' error
if grep -q 'SUGGESTIONS' error; then exit 1; fi
reject --diag=learn terminal.wnd
grep -Fx 'EXPLANATION' error
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
grep -Fx '  │                 ─── ожидается `int`' error
grep -Fx '  │            ^^^^^ получен `str`' error
grep -Fx '  │            ^^^^^^ получен `int`' error
reject --diag=learn --check returns.wnd
grep -F 'return int(text)' error
grep -Fx '  wind --explain W2001' error
"$wind" --explain W2001 > explain
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

printf 'func answer() -> int\n    return int("42")\nend\n' > good.wnd
PATH=/nonexistent "$wind" --check good.wnd
test ! -e output_ast.c
test ! -e app
printf '%s\n' 'Diagnostics: OK'
