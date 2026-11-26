#!/bin/sh
set -eu

wind=$(realpath "${1:-./wind}")
runtime=$(realpath runtime)
tmp=$(mktemp -d)
trap 'rm -f "$tmp"/*.wnd "$tmp/app" "$tmp/app-sanitized" "$tmp/output_ast.c"; rmdir "$tmp"' EXIT
cd "$tmp"

cat > exceptions.wnd <<'EOF'
func stop(str.reason) -> str
    str.list.bits = ["dead", reason + "!"]
    throw bits[1]
end

func choose(int.n) -> int
    try
        return n + 1
    catch unused
        return 0
    end
end

str.saved = "before"
int.counter = 0
try
    try
        counter = 7
        saved = "after" + " jump"
        str.unused = "${saved} ${stop(\"problem\")}"
    catch inner
        throw inner + " again"
    end
catch outer
    terminal.paste -> "${saved} ${counter}: ${outer}"
end

for i in 0..80
    try
        counter = choose(i)
        if i < 79
            continue
        end
        break
    catch
        terminal.paste -> "unexpected"
    end
end

try
    throw "done"
catch last
    terminal.paste -> "${last}: ${counter}"
end
EOF

expected='after jump 7: problem! again
done: 80'
"$wind" --wgc exceptions.wnd
test "$(./app)" = "$expected"
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
    -I "$runtime" output_ast.c "$runtime/gc.c" "$runtime/strings_gc.c" \
    "$runtime/collections_gc.c" -o app-sanitized
test "$(WGC_STRESS=1 ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./app-sanitized)" = "$expected"

printf '%s\n' 'str.msg = "Wind" + "!"' 'return msg.len' > exit.wnd
"$wind" exit.wnd
status=0
./app || status=$?
test "$status" = 5
printf '%s\n' 'WGC exceptions: OK'
