#!/bin/sh
set -eu
wind=$(realpath "${1:-./wind}")
tmp=$(mktemp -d)
trap 'find "$tmp" -type f -delete; find "$tmp" -depth -type d -exec rmdir {} +' EXIT
cd "$tmp"
mkdir bindings
export CPATH="$tmp"

cat > wind_borrow_fixture.h <<'EOF'
static inline int wind_fill(int *a, int n) {
    for (int i = 0; i < n; i++) a[i] = i + 40;
    return n;
}
static inline double wind_last(int n, const double *a) { return a[n - 1]; }
typedef int (*WindTransform)(int);
static inline int wind_apply(WindTransform callback, int x) { return callback(x); }
static WindTransform wind_saved;
static inline void wind_retain(WindTransform callback) { wind_saved = callback; }
static inline int wind_later(void) { return wind_saved(0); }
static inline int wind_combined(int n, const int *a, WindTransform callback) {
    return callback(a[n - 1]);
}
static inline double wind_map(double (*callback)(double), double x) { return callback(x); }
static inline void wind_visit(void (*callback)(int), int x) { callback(x); }
EOF
cat > bindings/borrow.windbind <<'EOF'
header <wind_borrow_fixture.h>
link "m"
borrow_buffer wind_fill 1 2
borrow_buffer wind_last 2 1
borrow_buffer wind_combined 2 1
borrow_callback wind_combined 3
borrow_callback wind_apply 1
borrow_callback wind_retain 1
borrow_callback wind_map 1
borrow_callback wind_visit 1
EOF
cat > works.wnd <<'EOF'
c.import <borrow>
func inc(int.n) -> int
    return n + 1
end
func transform(int.n) -> int
    try
        throw "handled" + " inside"
    catch
        return borrow.wind_apply(inc, n)
    end
end
func scale(frac.x) -> frac
    return x * 2,0
end
func show(int.n) -> void
    terminal.paste -> n
end
int.nums[3]
frac.reals[2]
reals[1] = 2,5
var count = borrow.wind_fill(int.nums)
terminal.paste -> "${count} ${nums[2]} ${borrow.wind_last(reals)} ${borrow.wind_combined(nums, transform)} ${borrow.wind_map(scale, 2,5)}"
borrow.wind_visit(show, 7)
EOF
if ! "$wind" --check --diag=compact works.wnd > /dev/null 2> error; then
    if grep -Fq 'automatic bindings require libclang' error; then
        printf '%s\n' 'Borrowed FFI: skipped (libclang unavailable)'
        exit 0
    fi
    cat error >&2
    exit 1
fi

cat > escape.wnd <<'EOF'
c.import <borrow>
func fail(int.n) -> int
    throw "outside"
end
try
    borrow.wind_apply(fail, 1)
catch
    terminal.paste -> "must not catch"
end
EOF
cat > retain.wnd <<'EOF'
c.import <borrow>
func inc(int.n) -> int
    return n + 1
end
borrow.wind_retain(inc)
borrow.wind_later()
EOF
reject_run() {
    "$wind" --diag=compact "$1" "$2"
    status=0
    ./app > output 2> error || status=$?
    test "$status" -eq 1
    test ! -s output
    grep -F "$3" error
}
for mode in --wgc --boehm; do
    "$wind" --diag=compact "$mode" works.wnd
    test "$(WGC_STRESS=1 ./app)" = '3 42 2.5 43 5
7'
    reject_run "$mode" escape.wnd 'exception cannot escape a C callback'
    reject_run "$mode" retain.wnd 'C callback called outside its native call or on another thread'
done

reject_check() {
    status=0
    "$wind" --check --diag=compact bad.wnd > /dev/null 2> error || status=$?
    test "$status" -eq 1
    grep -F "$1" error
}
printf '%s\n' 'c.import <borrow>' 'int.nums[3]' 'borrow.wind_fill(nums, 99)' > bad.wnd
reject_check 'expects 1 argument(s), got 2'
printf '%s\n' 'c.import <borrow>' 'frac.nums[3]' 'borrow.wind_fill(nums)' > bad.wnd
reject_check 'C buffer requires a named array of the matching element type'
printf '%s\n' 'c.import <borrow>' 'borrow.wind_apply(1, 2)' > bad.wnd
reject_check 'C callback requires a named Wind function with the exact signature'
sed '/borrow_callback wind_apply/d' bindings/borrow.windbind > bindings/no_rule.windbind
printf '%s\n' 'c.import <no_rule>' 'no_rule.wind_apply(1, 2)' > bad.wnd
reject_check 'needs `borrow_callback`'
sed 's/borrow_buffer wind_fill 1 2/borrow_buffer wind_fill 1 1/' bindings/borrow.windbind > bindings/bad_rule.windbind
printf '%s\n' 'c.import <bad_rule>' 'bad_rule.wind_fill(1)' > bad.wnd
reject_check 'ownership rule does not match C function'
printf '%s\n' 'Borrowed FFI: OK'
