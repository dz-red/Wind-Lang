#!/bin/sh
set -eu

wind=$(realpath "${1:-./wind}")
examples=$(dirname "$(realpath "$0")")/../examples/ffi
tmp=$(mktemp -d)
trap 'find "$tmp" -type f -delete; find "$tmp" -depth -type d -exec rmdir {} +' EXIT
cd "$tmp"
mkdir bindings
cp "$examples/bindings/math.windbind" bindings/
printf '%s\n' 'c.import <math>' \
    'terminal.paste -> "${math.pow(math.sqrt(4,0), 8,0)} ${math.sqrt(9,0)}"' > math.wnd

if ! PATH=/nonexistent "$wind" --check --diag=compact math.wnd > /dev/null 2> error; then
    if grep -Fq 'automatic bindings require libclang' error; then
        printf '%s\n' 'Automatic FFI: skipped (libclang unavailable)'
        exit 0
    fi
    cat error >&2
    exit 1
fi
test ! -e output_ast.c
for mode in --wgc --boehm; do
    "$wind" --diag=compact "$mode" math.wnd
    test "$(WGC_STRESS=1 ./app)" = '256 3'
    test "$(grep -Fc ') = &sqrt;' output_ast.c)" = 1
    test "$(grep -Fc ') = &pow;' output_ast.c)" = 1
done

reject() {
    status=0
    "$wind" --check --diag=compact "$1" > /dev/null 2> error || status=$?
    test "$status" = 1
    grep -F "$2" error
}

printf '%s\n' 'c.import <math>' 'math.pow()' > bad.wnd
reject bad.wnd '`math.pow()` expects 2 argument(s), got 0'
printf '%s\n' 'c.import <math>' 'math.sqrt(5)' > bad.wnd
reject bad.wnd 'expects `frac`, found `int`'
printf '%s\n' 'c.import <math>' 'math.no_such_function()' > bad.wnd
reject bad.wnd 'C header <math.h> does not declare function `no_such_function`'

export CPATH="$tmp"
cat > ffi_auto_fixture.h <<'EOF'
typedef int WindFixtureInt;
static inline WindFixtureInt wind_fixture_inc(WindFixtureInt x) { return x + 1; }
static inline const char *wind_fixture_echo(const char *text) { return text; }
int wind_fixture_variadic(int x, ...);
char *wind_fixture_raw(void);
EOF
cat > bindings/fixture.windbind <<'EOF'
header <ffi_auto_fixture.h>
link "m"
copy_cstr wind_fixture_echo
borrow_cstr wind_fixture_echo 1
EOF
cat > fixture.wnd <<'EOF'
c.import <fixture>
str.result = fixture.wind_fixture_echo("Wi" + "nd")
terminal.paste -> "${result} ${fixture.wind_fixture_inc(41)}"
EOF
"$wind" --diag=compact fixture.wnd
test "$(WGC_STRESS=1 ./app)" = 'Wind 42'

printf '%s\n' 'c.import <fixture>' 'fixture.wind_fixture_variadic(1)' > bad.wnd
reject bad.wnd 'has a signature Wind cannot import automatically'
printf '%s\n' 'c.import <fixture>' 'fixture.wind_fixture_raw()' > bad.wnd
reject bad.wnd 'has a signature Wind cannot import automatically'

sed '/^borrow_cstr/d' bindings/fixture.windbind > bindings/view.windbind
printf '%s\n' 'c.import <view>' 'view.wind_fixture_echo("Wind")' > bad.wnd
reject bad.wnd 'needs `borrow_cstr`'
sed '/^copy_cstr/d' bindings/fixture.windbind > bindings/view.windbind
reject bad.wnd 'needs `copy_cstr`'

cat > bindings/mixed.windbind <<'EOF'
header <ffi_auto_fixture.h>
link "m"
auto
func plus(int.x) -> int = wind_fixture_inc
EOF
printf '%s\n' 'c.import <mixed>' \
    'terminal.paste -> "${mixed.plus(1)} ${mixed.wind_fixture_inc(9)}"' > mixed.wnd
"$wind" --diag=compact mixed.wnd
test "$(./app)" = '2 10'

printf '%s\n' 'header <wind_auto_unavailable.h>' 'link "m"' > bindings/missing.windbind
printf '%s\n' 'c.import <missing>' 'missing.get()' > bad.wnd
reject bad.wnd 'error[W5003]: C header not found'

printf '%s\n' 'Automatic FFI: OK'
