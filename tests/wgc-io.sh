#!/bin/sh
set -eu

wind=$(realpath "${1:-./wind}")
runtime=$(realpath runtime)
tmp=$(mktemp -d)
trap 'rm -f "$tmp"/*.wnd "$tmp/app" "$tmp/app-sanitized" "$tmp/output_ast.c" "$tmp/data.json"; rmdir "$tmp"' EXIT
cd "$tmp"

cat > io.wnd <<'EOF'
link "m"
file.write("data.json", "{\"a\":1,\"b\":2,\"c\":3,\"d\":4,")
file.append("data.json", "\"long_key_for_buffer_growth\":[1,2,3,4,5,{\"name\":\"a long string for buffer growth\"}]}")
str.raw = file.read("data.json")
var doc = json.decode(raw)
var child = doc.get("long_key_for_buffer_growth").at(5)
doc = json.decode("null")
raw = "over" + "written"
str.name = child.get("name").str()
str.list.words = ["foo", name]
terminal.paste -> file.exists("data.json")
terminal.paste -> name
terminal.paste -> json.encode(child)
terminal.paste -> json.encode(words)
frac.root = sqrt(81)
terminal.paste -> root
EOF

expected='1
a long string for buffer growth
{"name":"a long string for buffer growth"}
["foo","a long string for buffer growth"]
9'

"$wind" --boehm io.wnd
test "$(./app)" = "$expected"
"$wind" --wgc io.wnd
test "$(./app)" = "$expected"
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
    -I "$runtime" output_ast.c "$runtime/gc.c" "$runtime/strings_gc.c" \
    "$runtime/collections_gc.c" -o app-sanitized
test "$(WGC_STRESS=1 ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./app-sanitized)" = "$expected"

python3 - "$wind" <<'PY'
from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from select import select
from socket import socket
from subprocess import PIPE, Popen, check_call, check_output
from sys import argv
from threading import Thread


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.reply(b"GET " + self.path.encode())

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        self.reply(b"POST " + body)

    def reply(self, body):
        self.send_response(200)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


with HTTPServer(("127.0.0.1", 0), Handler) as server:
    worker = Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        port = server.server_port
        Path("http.wnd").write_text(f'''
str.url = "http://127.0.0.1:{port}/example"
str.received = http.get(url)
terminal.paste -> received
terminal.paste -> http.post(url, "W" + "GC")
''')
        for args in ([], ["--boehm"]):
            check_call([argv[1], *args, "http.wnd"], timeout=30)
            assert check_output(["./app"], timeout=10) == b"GET /example\nPOST WGC\n"
    finally:
        server.shutdown()
        worker.join()

with socket() as s:
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
Path("serve.wnd").write_text(f'''
func answer() -> str
    str.list.words = ["W" + "GC", "сервер"]
    return json.encode(words)
end
http.serve {port}
    on "/wgc" -> answer()
end
''')
check_call([argv[1], "serve.wnd"], timeout=30)
with Popen(["./app"], stderr=PIPE) as app:
    try:
        assert select([app.stderr], [], [], 5)[0]
        assert b"listening" in app.stderr.readline()
        for _ in range(12):
            conn = HTTPConnection("127.0.0.1", port, timeout=5)
            try:
                conn.request("GET", "/wgc")
                assert conn.getresponse().read().decode() == '["WGC","сервер"]'
            finally:
                conn.close()
    finally:
        app.terminate()
        app.wait(timeout=5)
PY
printf '%s\n' 'WGC JSON, files and HTTP: OK'
