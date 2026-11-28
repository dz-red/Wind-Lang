#!/bin/sh
set -eu
wind=$(realpath "${1:-./wind}")
tmp=$(mktemp -d)
trap 'find "$tmp" -type f -delete; find "$tmp" -depth -type d -exec rmdir {} +' EXIT
cd "$tmp"
mkdir bindings
export CPATH="$tmp"

cat > wind_records_fixture.h <<'EOF'
struct point { int x; double y; };
typedef struct point Point;
typedef struct { int x; double y; } Other;
typedef Other OtherAlias;
struct unsupported { const char *text; };
struct bitfield { int bits : 3; };
struct nested { int x; union { double f; void *ptr; }; };
union either { int x; double y; };
static inline Point make_point(int x, double y) { return (Point){x, y}; }
static inline struct point move_point(struct point p, int x) { p.x += x; return p; }
static inline Other make_other(void) { return (Other){0, 0.0}; }
static inline int other_x(OtherAlias p) { return p.x; }
struct unsupported make_unsupported(void);
struct bitfield make_bitfield(void);
struct nested make_nested(void);
union either make_union(void);
struct point *get_pointer(void);

struct mismatch {
#ifdef __clang__
    int x;
    double y;
#else
    double y;
    int x;
#endif
};
static inline struct mismatch make_mismatch(void) { return (struct mismatch){.x = 1, .y = 2.0}; }
EOF
cat > bindings/records.windbind <<'EOF'
header <wind_records_fixture.h>
link "m"
EOF
cat > works.wnd <<'EOF'
c.import <records>
var point = records.make_point(4, 2,5)
var copy = point
copy.x = 40
copy = records.move_point(copy, 2)
var other = records.make_other()
other.x = point.x
terminal.paste -> "${point.x} ${copy.x} ${copy.y} ${records.move_point(point, 1).x} ${records.other_x(other)}"
EOF
if ! "$wind" --check --diag=compact works.wnd > /dev/null 2> error; then
    if grep -Fq 'automatic bindings require libclang' error; then
        printf '%s\n' 'C records: skipped (libclang unavailable)'
        exit 0
    fi
    cat error >&2
    exit 1
fi
for mode in --wgc --boehm; do
    "$wind" --diag=compact "$mode" works.wnd
    test "$(WGC_STRESS=1 ./app)" = '4 42 2.5 5 4'
done

reject() {
    status=0
    "$wind" "$1" --diag=compact bad.wnd > /dev/null 2> error || status=$?
    test "$status" -eq 1
    grep -F "$2" error
}
printf '%s\n' 'c.import <records>' 'var point = records.make_point(4, 2,5)' > prefix.wnd
cp prefix.wnd bad.wnd
printf '%s\n' 'terminal.paste -> point.z' >> bad.wnd
reject --check 'has no field `z`'
cp prefix.wnd bad.wnd
printf '%s\n' 'point.x = 2,5' >> bad.wnd
reject --check 'C record field assignment requires'
cp prefix.wnd bad.wnd
printf '%s\n' 'point = records.make_other()' >> bad.wnd
reject --check 'matching record types'
cp prefix.wnd bad.wnd
printf '%s\n' 'records.other_x(point)' >> bad.wnd
reject --check 'found `struct point`'
cp prefix.wnd bad.wnd
printf '%s\n' 'terminal.paste -> point + 1' >> bad.wnd
reject --check 'C record values cannot be used with operators'
for function in make_unsupported make_bitfield make_nested make_union get_pointer; do
    printf '%s\n' 'c.import <records>' "records.$function()" > bad.wnd
    reject --check 'has a signature Wind cannot import automatically'
done
printf '%s\n' 'c.import <records>' 'var value = records.make_mismatch()' 'terminal.paste -> value.x' > bad.wnd
reject --wgc 'error[W5005]'
printf '%s\n' 'C records: OK'
