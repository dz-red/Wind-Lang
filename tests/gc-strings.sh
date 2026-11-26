#!/bin/sh
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
func spin(str.seed, int.n) -> str
    str.last = seed
    for i in 0..n
        str.scoped = "${seed}:${i}"
        if i == 0
            continue
        end
        last = scoped
        if i == n - 1
            break
        end
    end
    return last
end
func recurse(str.token, int.depth) -> str
    str.current = "${token}:${depth}"
    if depth == 0
        return current
    end
    return wrap(current, recurse(token, depth - 1))
end

str.label = label + " local"
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
int.checks = 0
while len(str(checks)) > 0 && checks < 32
    checks += 1
    continue
end
terminal.paste -> "${read_global()} ${label} ${alias} ${kept} ${len(empty)}"
terminal.paste -> wrap(spin("left", 500), spin("right", 500))
terminal.paste -> recurse("r", 2)
EOF

"$wind" --gc-strings strings.wnd
expected='global! <<<global! local|42>|42>|42> <ab|cd> 42 0
<left:499|right:499>
<r:2|<r:1|r:0>>'
test "$(./app)" = "$expected"
if readelf -d app | grep -Fq 'libgc.so'; then
    printf '%s\n' 'Unexpected dependency on Boehm GC' >&2
    exit 1
fi

# Ограничение проверяет сборку внутри циклов, а не только при выходе.
cat > wrap.c <<'EOF'
#include <assert.h>
#include "gc.h"

void *__real_wind_gc_alloc(WindGc *, size_t);
size_t __real_wind_gc_collect_with(WindGc *, const void *, WindGcTrace);
static size_t live;
static size_t collected;

__attribute__((destructor)) static void check_collection(void) {
    assert(collected >= 1000);
}

void *__wrap_wind_gc_alloc(WindGc *gc, size_t size) {
    void *data = __real_wind_gc_alloc(gc, size);
    if (data) ++live;
    assert(live < 64);
    return data;
}

size_t __wrap_wind_gc_collect_with(WindGc *gc, const void *roots, WindGcTrace trace) {
    size_t freed = __real_wind_gc_collect_with(gc, roots, trace);
    assert(freed <= live);
    live -= freed;
    collected += freed;
    return freed;
}
EOF
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
    -I "$runtime" output_ast.c "$runtime/gc.c" "$runtime/strings_gc.c" wrap.c \
    "$runtime/collections_gc.c" \
    -Wl,--wrap=wind_gc_alloc,--wrap=wind_gc_collect_with -o app-sanitized
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

reject 'str.value = getenv("HOME")' "unknown function 'getenv'"
printf '%s\n' 'String GC: OK'
