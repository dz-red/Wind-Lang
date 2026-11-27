#!/bin/sh
set -eu

wind=$(realpath "${1:-./wind}")
tmp=$(mktemp -d)
trap 'rm -f "$tmp"/*.wnd "$tmp/output_ast.c" "$tmp/app" "$tmp/probe" "$tmp/error"; rmdir "$tmp"' EXIT
cd "$tmp"

reject() {
    if "$wind" --check --diag=compact bad.wnd > /dev/null 2> error; then
        printf 'Unexpected success: %s\n' "$1" >&2
        exit 1
    fi
    grep -F "$1" error > /dev/null
}

printf '%s\n' 'c.import <sqlite3>' > bad.wnd
reject 'bad.wnd:1:11: error[W3004]: unknown C binding `sqlite3`'
printf '%s\n' 'terminal.paste -> sqlite.version()' > bad.wnd
reject 'error[W3004]: C binding `sqlite` is not imported'
printf '%s\n' 'c.import <sqlite>' 'terminal.paste -> sqlite.open()' > bad.wnd
reject 'error[W3004]: C binding `sqlite` has no function `open`'
printf '%s\n' 'c.import <sqlite>' 'terminal.paste -> "${sqlite.version(1)}"' > bad.wnd
reject 'error[W3004]: `sqlite.version()` expects no arguments'
printf '%s\n' 'c.import <sqlite>' 'sqlite3_libversion()' > bad.wnd
reject "unknown function 'sqlite3_libversion'"
printf '%s\n' 'c.import <sqlite' > bad.wnd
reject "expected '>' after C binding name"
printf '%s\n' 'if true' '    c.import <sqlite>' 'end' > bad.wnd
reject '`c.import` is only allowed at the top level'

cat > db.wnd <<'EOF'
c.import <sqlite>
func db_version() -> str
    return sqlite.version()
end
EOF
cat > imported.wnd <<'EOF'
take db_version from db
terminal.paste -> db_version()
EOF
cat > direct.wnd <<'EOF'
c.import <sqlite>
c.import <sqlite>
var version = sqlite.version()
terminal.paste -> "${version}"
EOF
PATH=/nonexistent "$wind" --check direct.wnd
PATH=/nonexistent "$wind" --check imported.wnd
test ! -e output_ast.c
test ! -e app
test "$("$wind" --ast direct.wnd | grep -c 'C import <sqlite>')" = 1

if ! printf '%s\n' '#include <sqlite3.h>' 'int main(void) { return !sqlite3_libversion(); }' |
    gcc -x c - -lsqlite3 -o probe > /dev/null 2>&1; then
    printf '%s\n' 'C imports: checks OK; linking skipped (SQLite development library unavailable)'
    exit 0
fi

printf '%s\n' 'link "sqlite3"' 'terminal.paste -> sqlite3_libversion()' > legacy.wnd
printf '%s\n' 'link "sqlite3"' 'c.import <sqlite>' 'terminal.paste -> sqlite.version()' > mixed.wnd
"$wind" --wgc legacy.wnd
version=$(./app)
printf '%s\n' "$version" | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' > /dev/null
for mode in --wgc --boehm; do
    for program in direct.wnd imported.wnd legacy.wnd mixed.wnd; do
        "$wind" "$mode" "$program"
        test "$(WGC_STRESS=1 ./app)" = "$version"
        test "$(grep -c '^#include <sqlite3.h>$' output_ast.c)" = 1
        grep -F 'wstr_dup(sqlite3_libversion())' output_ast.c > /dev/null
        readelf -d app | grep -F 'libsqlite3.so' > /dev/null
        if test "$mode" = --wgc && readelf -d app | grep -Fq 'libgc.so'; then exit 1; fi
    done
done

printf '%s\n' 'C imports: OK'
