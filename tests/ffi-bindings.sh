#!/bin/sh
set -eu

wind=$(realpath "${1:-./wind}")
examples=$(dirname "$(realpath "$0")")/../examples/ffi
tmp=$(mktemp -d)
trap 'find "$tmp" -type f -delete; find "$tmp" -depth -type d -exec rmdir {} +' EXIT
cd "$tmp"
export XDG_DATA_HOME="$tmp/user share"
mkdir -p project/bindings "$XDG_DATA_HOME/wind/bindings"
cp "$examples/bindings/math.windbind" project/bindings/
cp "$examples/bindings/zlib.windbind" "$XDG_DATA_HOME/wind/bindings/"

reject() {
    status=0
    "$wind" "$@" > /dev/null 2> error || status=$?
    test "$status" = 1
}

cat > project/main.wnd <<'EOF'
c.import <math>
c.import <math>
frac.result = math.pow(2,0, 8,0)
terminal.paste -> "${result} ${math.sqrt(9,0)} ${math.cos(0,0)} ${math.sin(0,0)}"
EOF
PATH=/nonexistent "$wind" --check project/main.wnd
test ! -e output_ast.c
for mode in --wgc --boehm; do
    "$wind" --diag=compact "$mode" project/main.wnd
    test "$(WGC_STRESS=1 ./app)" = '256 3 1 0'
done

printf '%s\n' 'c.import <zlib>' 'terminal.paste -> zlib.version()' > project/zlib.wnd
PATH=/nonexistent "$wind" --check project/zlib.wnd
if printf '%s\n' '#include <zlib.h>' 'int main(void) { return !zlibVersion(); }' |
    gcc -x c - -lz -o probe >/dev/null 2>&1; then
    "$wind" --diag=compact project/zlib.wnd
    WGC_STRESS=1 ./app | grep -Eq '^[0-9]+\.[0-9]+'
    "$wind" --diag=compact --boehm project/zlib.wnd
    ./app | grep -Eq '^[0-9]+\.[0-9]+'
fi

printf '%s\n' 'bad project binding' > project/bindings/zlib.windbind
reject --check --diag=compact project/zlib.wnd
grep -F 'project/bindings/zlib.windbind:1:1: error[W5002]' error

printf '%s\n' 'c.import <wind_ffi_missing>' > project/bad.wnd
reject --check --diag=learn project/bad.wnd
grep -F 'error[W5001]' error
grep -Fx 'SEARCH PATHS' error
grep -Fx '  project/bindings/wind_ffi_missing.windbind' error
grep -Fx "  $XDG_DATA_HOME/wind/bindings/wind_ffi_missing.windbind" error
grep -Fx '  /usr/share/wind/bindings/wind_ffi_missing.windbind' error
"$wind" --explain W5001 > explain
grep -F 'ownership rules' explain

mkdir -p "$tmp/home/.local/share/wind/bindings"
cp "$examples/bindings/math.windbind" "$tmp/home/.local/share/wind/bindings/home_math.windbind"
cat > project/helper.wnd <<'EOF'
c.import <home_math>
func result() -> frac
    return home_math.sqrt(9,0)
end
EOF
printf '%s\n' 'use helper' 'terminal.paste -> result()' > project/module.wnd
XDG_DATA_HOME= HOME="$tmp/home" "$wind" --diag=compact project/module.wnd
test "$(./app)" = 3

printf '%s\n' 'c.import <math>' 'terminal.paste -> math.sqrt(9)' > project/bad.wnd
reject --check --diag=compact project/bad.wnd
grep -F 'argument 1 of `math.sqrt()` expects `frac`, found `int`' error
printf '%s\n' 'c.import <math>' 'terminal.paste -> math.pow(2,0)' > project/bad.wnd
reject --check --diag=compact project/bad.wnd
grep -F '`math.pow()` expects 2 argument(s), got 1' error

printf '%s\n' 'c.import <wrong>' 'wrong.sqrt(9)' > project/bad.wnd
printf '%s\n' 'header <math.h>' 'link "m"' 'func sqrt(int.x) -> int = sqrt' > project/bindings/wrong.windbind
PATH=/nonexistent "$wind" --check project/bad.wnd
reject --diag=compact project/bad.wnd
grep -F 'project/bindings/wrong.windbind:3:27: error[W5005]' error

printf '%s\n' 'header <wind_ffi_missing.h>' 'link "m"' 'func sqrt(int.x) -> int = sqrt' > project/bindings/wrong.windbind
reject --diag=compact project/bad.wnd
grep -F 'project/bindings/wrong.windbind:1:9: error[W5003]' error

printf '%s\n' 'header <stdlib.h>' 'link "wind_ffi_missing"' 'func sqrt(int.x) -> int = abs' > project/bindings/wrong.windbind
reject --diag=compact project/bad.wnd
grep -F 'project/bindings/wrong.windbind:2:6: error[W5004]' error

printf '%s\n' 'header <fixture_missing_symbol.h>' 'link "m"' \
    'func sqrt(int.x) -> int = wind_test_not_linked' > project/bindings/wrong.windbind
printf '%s\n' 'int wind_test_not_linked(int);' > fixture_missing_symbol.h
export CPATH="$tmp"
reject --diag=compact project/bad.wnd
grep -F 'wind_test_not_linked' error
if grep -Fq 'error[W5004]' error; then exit 1; fi

cat > fixture.h <<'EOF'
static inline int wind_test_int(int x) { return x + 1; }
static inline void wind_test_void(int x) { (void)x; }
static inline const char *wind_test_null(void) { return 0; }
EOF
cat > project/bindings/fixture.windbind <<'EOF'
header <fixture.h>
link "m"
func inc(int.x) -> int = wind_test_int
func run(int.x) -> void = wind_test_void
func missing() -> str = wind_test_null copy_cstr
EOF
cat > project/fixture.wnd <<'EOF'
c.import <fixture>
fixture.run(12)
terminal.paste -> fixture.inc(41)
terminal.paste -> fixture.missing()
EOF
"$wind" --diag=compact project/fixture.wnd
status=0
./app > result 2> error || status=$?
test "$status" = 1
grep -Fx '42' result
grep -Fx "Wind: C function 'wind_test_null' returned NULL" error

printf '%s\n' 'FFI bindings: OK'
