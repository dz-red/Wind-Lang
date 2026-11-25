#!/bin/sh
# Linux: run from the repository root after make.
set -eu

wind=$(realpath "${1:-./wind}")
runtime=$(realpath runtime)
tmp=$(mktemp -d)
trap 'rm -f "$tmp"/*.wnd "$tmp/wrap.c" "$tmp/output_ast.c" "$tmp/app" "$tmp/app-sanitized" "$tmp/error"; rmdir "$tmp"' EXIT
cd "$tmp"

cat > strings.wnd <<'EOF'
global str.label = "global" + "!"
func read_global() -> str
    return label
end
func wrap(str.left, str.right) -> str
    return "<${left}|${right}>"
end

str.label = "local" + "!"
str.kept = wrap("a" + "b", "c" + "d")
str.alias = kept
kept = str(42)
str.empty
repeat 3
    str.temp = wrap(label, kept)
    label = temp
end
for label in 0..1
    str.scoped = str(label)
end
terminal.paste -> "${read_global()} ${label} ${alias} ${kept} ${len(empty)}"
EOF

"$wind" --gc-strings strings.wnd
expected='global! <<<local!|42>|42>|42> <ab|cd> 42 0'
test "$(./app)" = "$expected"
if readelf -d app | grep -Fq 'libgc.so'; then
    printf '%s\n' 'Unexpected dependency on Boehm GC' >&2
    exit 1
fi

# Observe the generated checkpoints, not just the final destroy.
cat > wrap.c <<'EOF'
#include <assert.h>
#include "gc.h"

size_t __real_wind_gc_collect(WindGc *, const void *const *, size_t);
size_t __wrap_wind_gc_collect(WindGc *gc, const void *const *roots, size_t n);
static size_t calls;

__attribute__((destructor)) static void check_calls(void) {
    assert(calls == 8);
}

size_t __wrap_wind_gc_collect(WindGc *gc, const void *const *roots, size_t n) {
    size_t freed = __real_wind_gc_collect(gc, roots, n);
    if (++calls == 2)
        assert(freed == 2); /* Both temporary arguments of wrap are gone. */
    if (calls == 8)
        assert(freed == 1); /* The temporary interpolation used by print. */
    return freed;
}
EOF
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
    -I "$runtime" output_ast.c "$runtime/gc.c" "$runtime/strings_gc.c" wrap.c \
    -Wl,--wrap=wind_gc_collect -o app-sanitized
test "$(ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./app-sanitized)" = "$expected"

reject() {
    printf '%s\n' "$1" > bad.wnd
    if "$wind" --gc-strings bad.wnd > /dev/null 2> error; then
        printf 'Unexpected success: %s\n' "$1" >&2
        exit 1
    fi
    grep -F '[codegen error]' error > /dev/null
    grep -F "$2" error > /dev/null
}

reject 'str.list.values = []' 'only supports scalar declarations'
reject 'var value = json.decode("null")' 'does not support the json module'
reject 'str.value = getenv("HOME")' "unknown function 'getenv'"
printf '%s\n' 'String GC: OK'
