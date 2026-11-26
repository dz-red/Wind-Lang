#!/bin/sh
set -eu

wind=$(realpath "${1:-./wind}")
runtime=$(realpath runtime)
tmp=$(mktemp -d)
trap 'rm -f "$tmp"/*.wnd "$tmp/app" "$tmp/app-sanitized" "$tmp/output_ast.c"; rmdir "$tmp"' EXIT
cd "$tmp"

cat > collections.wnd <<'EOF'
func label(int.n) -> str
    str.text = "элемент ${n}"
    repeat 2
        str.tmp = "${text}:${n}"
    end
    return text
end

str.list.words = [label(1), label(2)]
str.saved[2]
dict[str,str].entries = [label(0): label(3)]

for i in 0..128
    words.add(label(i))
end
saved[0] = words[5]
saved[1] = words[129]
entries[label(0)] = words[1]
entries[label(4)] = words[0]
words[0] = label(8)
repeat 128
    words.pop()
end

str.joined = ""
loop word in words
    joined = joined + "${word}|"
end
int.key_bytes = 0
loop k in entries
    key_bytes += k.len
end
terminal.paste -> "${saved[0]} ${saved[1]}"
terminal.paste -> "${joined} ${entries[label(0)]} ${entries[label(4)]} ${entries.len} ${len(words)} ${key_bytes}"

int.list.nums = [1, 2]
frac.list.fracs = [1,5, 2,5]
dict[int,frac].numeric = [2: 3,5]
terminal.paste -> "${nums[1]} ${fracs[1]} ${numeric[2]}"
EOF

expected='элемент 3 элемент 127
элемент 8|элемент 2| элемент 2 элемент 1 2 2 32
2 2.5 3.5'
"$wind" --boehm collections.wnd
test "$(./app)" = "$expected"

check_wgc() {
    "$wind" "$1"
    test "$(./app)" = "$2"
    if readelf -d app | grep -Fq 'libgc.so'; then exit 1; fi
    gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
        -I "$runtime" output_ast.c "$runtime/gc.c" "$runtime/strings_gc.c" \
        "$runtime/collections_gc.c" -o app-sanitized
    test "$(WGC_STRESS=1 ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./app-sanitized)" = "$2"
}

check_wgc collections.wnd "$expected"

cat > global.wnd <<'EOF'
global str.list.words = ["a" + "b", "c" + "d"]
global dict[str,str].entries = ["g" + "c": "Wind" + "!"]
global str.saved[1]

func reset() -> str
    words[0] = "n" + "ew"
    entries["gc"] = "W" + "GC"
    return "x"
end

func pair(str.a, str.b) -> str
    return "${a}/${b}"
end

saved[0] = entries["gc"]
terminal.paste -> pair(reset(), words[0])
terminal.paste -> "${words[0]} ${entries[\"gc\"]} ${saved[0]}"
str.ascii = "W" + "GC"
terminal.paste -> ascii[1]
EOF

check_wgc global.wnd 'x/ab
new WGC Wind!
G'
printf '%s\n' 'WGC collections: OK'
