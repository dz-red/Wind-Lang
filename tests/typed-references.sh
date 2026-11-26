#!/bin/sh
set -eu

wind=$(realpath "${1:-./wind}")
tmp=$(mktemp -d)
trap 'rm -f "$tmp"/*.wnd "$tmp/error" "$tmp/app" "$tmp/output_ast.c"; rmdir "$tmp"' EXIT
cd "$tmp"

cat > good.wnd <<'EOF'
func square(int.x) -> int
    return int.x * x
end
int.n = 7
var inferred = 3
frac.rate = 2,9
int.nums[1]
nums[0] = 4
terminal.paste -> "${square(n)} ${square(int.n)} ${int.inferred} ${int(rate)} ${int.nums[0]}"
EOF
"$wind" good.wnd
test "$(./app)" = '49 49 3 2 4'

reject() {
    if "$wind" bad.wnd > /dev/null 2> error; then
        printf 'Unexpected success: %s\n' "$1" >&2
        exit 1
    fi
    grep -F 'error[W2002]' error > /dev/null
    grep -F "$1" error > /dev/null
}

# Explicit prefix is checked even in a call whose type is already known.
printf 'func f(int.x) -> int\n    return x\nend\nfrac.rate = 2,9\nint.y = f(int.rate)\n' > bad.wnd
reject "typed reference 'int.rate': 'rate' has type 'frac'"
printf 'terminal.paste -> "${str.missing}"\n' > bad.wnd
reject "typed reference 'str.missing': variable 'missing' is not declared"
printf 'int.nums[1]\nterminal.paste -> "${frac.nums[0]}"\n' > bad.wnd
reject "typed reference 'frac.nums': 'nums' has element type 'int'"

printf 'Typed references: OK\n'
