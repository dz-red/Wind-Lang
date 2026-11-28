#!/bin/sh
set -eu
wind=$(realpath "${1:-./wind}")
tmp=$(mktemp -d)
trap 'find "$tmp" -type f -delete; find "$tmp" -depth -type d -exec rmdir {} +' EXIT
cd "$tmp"
mkdir bindings
export CPATH="$tmp"

cat > bindings/memory.windbind <<'EOF'
header <string.h>
link "c"
borrow_bytes memcmp 1 3
borrow_bytes memcmp 2 3
EOF
cat > wind_bytes_fixture.h <<'EOF'
#include <stddef.h>
#include <string.h>

typedef size_t Count;
static inline int clear_words(unsigned char *bytes, Count size) {
    memset(bytes, 0, size);
    return size == 3 * sizeof(int);
}
static inline int last_word(size_t count, const int *words) { return words[count - 1]; }
static inline int byte_count(const unsigned char *bytes, int count) { return count + bytes[0]; }
static inline int mixed_units(const void *bytes, const int *words, int count) {
    return bytes != NULL && words != NULL && count > 0;
}
static inline int unrelated_size(size_t size) { return size != 0; }
static inline int copy_bytes(void *out, const void *in, Count size, int (*done)(int)) {
    memcpy(out, in, size);
    return done(size == 3 * sizeof(int));
}
EOF
cat > bindings/fixture.windbind <<'EOF'
header <wind_bytes_fixture.h>
link "c"
borrow_bytes clear_words 1 2
borrow_buffer last_word 2 1
borrow_bytes byte_count 1 2
borrow_bytes copy_bytes 1 3
borrow_bytes copy_bytes 2 3
borrow_callback copy_bytes 4
EOF
cat > works.wnd <<'EOF'
c.import <memory>
c.import <fixture>
func finish(int.ok) -> int
    return ok + 6
end
int.a[3]
int.b[3]
a[2] = 42
b[2] = 42
terminal.paste -> "${memory.memcmp(a, b)} ${fixture.last_word(a)}"
var cleared = fixture.clear_words(int.a)
terminal.paste -> "${cleared} ${a[2]} ${fixture.byte_count(a) > 3}"
var copied = fixture.copy_bytes(a, b, finish)
terminal.paste -> "${copied} ${a[2]}"
EOF
if ! "$wind" --check --diag=compact works.wnd > /dev/null 2> error; then
    if grep -Fq 'automatic bindings require libclang' error; then
        printf '%s\n' 'Byte buffers: skipped (libclang unavailable)'
        exit 0
    fi
    cat error >&2
    exit 1
fi

cat > uneven.wnd <<'EOF'
c.import <memory>
int.a[2]
int.b[3]
memory.memcmp(a, b)
EOF
for mode in --wgc --boehm; do
    "$wind" "$mode" --diag=compact works.wnd
    test "$(WGC_STRESS=1 ./app)" = '0 42
1 0 1
7 42'
    "$wind" "$mode" --diag=compact uneven.wnd
    status=0
    ./app > output 2> error || status=$?
    test "$status" -eq 1
    test ! -s output
    grep -F 'C buffers sharing a length must have equal lengths' error
done

reject() {
    status=0
    "$wind" --check --diag=compact bad.wnd > /dev/null 2> error || status=$?
    test "$status" -eq 1
    grep -F "$1" error
}
printf '%s\n' 'c.import <memory>' 'int.a[2]' 'memory.memcmp(a, a, 99)' > bad.wnd
reject 'expects 2 argument(s), got 3'
printf '%s\n' 'c.import <fixture>' 'fixture.clear_words("Wind")' > bad.wnd
reject 'C byte buffer requires a named'
printf '%s\n' 'c.import <fixture>' 'fixture.unrelated_size(1)' > bad.wnd
reject 'uses `size_t` outside a buffer length'
sed '/^borrow_bytes clear_words/d' bindings/fixture.windbind > bindings/no_rule.windbind
printf '%s\n' 'c.import <no_rule>' 'int.a[3]' 'no_rule.clear_words(a)' > bad.wnd
reject 'needs `borrow_bytes`'
sed 's/borrow_bytes clear_words/borrow_buffer clear_words/' bindings/fixture.windbind > bindings/bad_unit.windbind
printf '%s\n' 'c.import <bad_unit>' 'int.a[3]' 'bad_unit.clear_words(a)' > bad.wnd
reject 'ownership rule does not match C function'
cat >> bindings/fixture.windbind <<'EOF'
borrow_bytes mixed_units 1 3
borrow_buffer mixed_units 2 3
EOF
printf '%s\n' 'c.import <fixture>' 'int.a[3]' 'fixture.mixed_units(a, a)' > bad.wnd
reject 'ownership rule does not match C function'

printf '%s\n' 'Byte buffers: OK'
