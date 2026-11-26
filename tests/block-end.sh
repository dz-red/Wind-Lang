#!/bin/sh
set -eu

wind=$(realpath "${1:-./wind}")
tmp=$(mktemp -d)
trap 'rm -f "$tmp"/*.wnd "$tmp"/*.ast "$tmp/error" "$tmp/app" "$tmp/output_ast.c"; rmdir "$tmp"' EXIT
cd "$tmp"

cat > long.wnd <<'EOF'
func square(int.x) -> int
    if x > 0
        return x * x
    else
        return 0
    end if
end func
int.list.values = []
values.add(7)
while false
end while
repeat 1
    for i in 0..1
        for n in values
            terminal.paste -> "${square(n)}"
        end for
    end for
end repeat
loop n in values
end loop
try
    throw "test"
catch
end try
EOF
sed 's/end \(if\|func\|while\|for\|repeat\|loop\|try\)$/end/' long.wnd > short.wnd
"$wind" --ast long.wnd > long.ast
"$wind" --ast short.wnd > short.ast
cmp long.ast short.ast
"$wind" short.wnd
test "$(./app)" = 49

# Parsing the server is enough: don't start a listener. Exercise EOF without a newline.
printf 'http.serve 8080\n    on "/" -> "ok"\nend http.serve' > long.wnd
printf 'http.serve 8080\n    on "/" -> "ok"\nend' > short.wnd
"$wind" --ast long.wnd > long.ast
"$wind" --ast short.wnd > short.ast
cmp long.ast short.ast

reject() {
    if "$wind" --diag=compact --ast bad.wnd > /dev/null 2> error; then
        printf 'Unexpected success: %s\n' "$1" >&2
        exit 1
    fi
    grep -F "$1" error > /dev/null
}

printf 'if true\n    while false\n    end if\nend\n' > bad.wnd
reject "bad.wnd:3:9: error[W1002]: expected 'end' or 'end while'"
printf 'if true\nend if extra\n' > bad.wnd
reject "bad.wnd:2:8: error[W1002]: expected end of line"
printf 'http.serve 8080\nend http.other\n' > bad.wnd
reject "bad.wnd:2:10: error[W1002]: expected 'serve'"

printf 'Block endings: OK\n'
