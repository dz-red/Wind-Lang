#!/bin/sh
set -eu

wind=$(realpath "${1:-./wind}")
root=$(dirname "$(realpath "$0")")/..
tmp=$(mktemp -d)
trap 'find "$tmp" -type f -delete; find "$tmp" -depth -type d -exec rmdir {} +' EXIT
cd "$tmp"
mkdir bindings

export CPATH="$tmp"
cat > ffi_strings.h <<'EOF'
static inline const char *wind_test_echo(const char *text) { return text; }
EOF
cat > bindings/ffi_strings.windbind <<'EOF'
header <ffi_strings.h>
link "m"
func echo(str.text borrow_cstr) -> str = wind_test_echo copy_cstr
EOF
cat > string.wnd <<'EOF'
c.import <ffi_strings>
str.original = "Wind " + "GC"
str.copy = ffi_strings.echo(original + "!")
original = "overwritten"
terminal.paste -> copy
EOF
for mode in --wgc --boehm; do
    "$wind" --diag=compact "$mode" string.wnd
    test "$(WGC_STRESS=1 ./app)" = 'Wind GC!'
done

reject_check() {
    status=0
    "$wind" --check --diag=compact "$1" > /dev/null 2> error || status=$?
    test "$status" = 1
    grep -F "$2" error > /dev/null
}

printf '%s\n' 'c.import <ffi_strings>' 'ffi_strings.echo(1)' > bad.wnd
reject_check bad.wnd 'expects `str`, found `int`'
sed 's/ borrow_cstr//' bindings/ffi_strings.windbind > bindings/no_borrow.windbind
printf '%s\n' 'c.import <no_borrow>' > bad.wnd
reject_check bad.wnd '`str` parameters require `borrow_cstr`'
printf '%s\n' 'c.import <sqlite>' 'var db = sqlite.open(":memory:")' 'sqlite.finalize(db)' > bad.wnd
reject_check bad.wnd 'expects `sqlite.statement`, found `sqlite.connection`'
printf '%s\n' 'c.import <sqlite>' 'var db = sqlite.open(":memory:")' 'int.x = int(db)' > bad.wnd
reject_check bad.wnd 'C handles cannot be used as arguments to non-binding functions'
printf '%s\n' 'c.import <sqlite>' 'var db = sqlite.open(":memory:")' 'db = 42' > bad.wnd
reject_check bad.wnd "C handle assignment requires '=' and matching handle types"

PATH=/nonexistent "$wind" --check "$root/examples/sqlite-query.wnd"
if ! printf '%s\n' '#include <sqlite3.h>' 'int main(void) { return !sqlite3_libversion(); }' |
    gcc -x c - -lsqlite3 -o probe >/dev/null 2>&1; then
    printf '%s\n' 'FFI SQLite: checks OK; execution skipped (SQLite development library unavailable)'
    exit 0
fi

for mode in --wgc --boehm; do
    "$wind" --diag=compact "$mode" "$root/examples/sqlite-query.wnd"
    test "$(WGC_STRESS=1 ./app)" = 'Wind: 42'
done

cat > state.wnd <<'EOF'
c.import <sqlite>
var db = sqlite.open(":memory:")
sqlite.exec(db, "CREATE TABLE items(name TEXT, quantity INTEGER)")
var stmt = sqlite.prepare(db, "INSERT INTO items VALUES (?, NULL)")
str.name = "Wind " + "GC"
sqlite.bind_text(stmt, 1, name)
name = "cleared"
sqlite.step(stmt)
sqlite.step(stmt)
sqlite.reset(stmt)
sqlite.bind_text(stmt, 1, "second")
sqlite.step(stmt)
sqlite.finalize(stmt)

var query = sqlite.prepare(db, "SELECT name, quantity FROM items ORDER BY rowid")
str.first = ""
str.second = ""
int.count = 0
while sqlite.step(query)
    count += 1
    if count == 1 && sqlite.is_null(query, 1)
        first = sqlite.column_text(query, 0)
    else
        second = sqlite.column_text(query, 0)
    end
end
sqlite.finalize(query)
sqlite.close(db)
terminal.paste -> "${count}: ${first}, ${second}"
EOF
"$wind" --diag=compact state.wnd
test "$(WGC_STRESS=1 ./app)" = '2: Wind GC, second'

reject_run() {
    "$wind" --diag=compact "$1"
    status=0
    WGC_STRESS=1 ./app > /dev/null 2> error || status=$?
    test "$status" = 1
    grep -Fx "$2" error > /dev/null
}

cat > stale.wnd <<'EOF'
c.import <sqlite>
var db = sqlite.open(":memory:")
var alias = db
sqlite.close(db)
var next_db = sqlite.open(":memory:")
sqlite.exec(alias, "SELECT 1")
EOF
reject_run stale.wnd 'sqlite.exec: connection is closed or invalid'

cat > stale_stmt.wnd <<'EOF'
c.import <sqlite>
var db = sqlite.open(":memory:")
var stmt = sqlite.prepare(db, "SELECT 1")
var alias = stmt
sqlite.finalize(stmt)
var next_stmt = sqlite.prepare(db, "SELECT 2")
sqlite.step(alias)
EOF
reject_run stale_stmt.wnd 'sqlite.step: statement is finalized or invalid'

cat > busy.wnd <<'EOF'
c.import <sqlite>
var db = sqlite.open(":memory:")
var stmt = sqlite.prepare(db, "SELECT 1")
sqlite.close(db)
EOF
reject_run busy.wnd 'sqlite.close: finalize all statements before closing the connection'

cat > bounds.wnd <<'EOF'
c.import <sqlite>
var db = sqlite.open(":memory:")
var stmt = sqlite.prepare(db, "SELECT 2147483648")
sqlite.step(stmt)
terminal.paste -> sqlite.column_int(stmt, 0)
EOF
reject_run bounds.wnd 'sqlite.column_int: integer does not fit in Wind int'

printf '%s\n' 'FFI SQLite: OK'
