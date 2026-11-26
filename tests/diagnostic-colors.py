import errno
import os
import pty
import re
import subprocess
import sys

wind, source = sys.argv[1:]
env = dict(os.environ, TERM="xterm")
env.pop("NO_COLOR", None)
ansi = re.compile(rb"\x1b\[[0-9;]*m")


def run(flags, environment, terminal=False):
    master, slave = pty.openpty() if terminal else (None, None)
    try:
        proc = subprocess.run(
            [wind, *flags, source], env=environment, stdout=subprocess.DEVNULL,
            stderr=slave if terminal else subprocess.PIPE, timeout=5,
        )
        assert proc.returncode == 1
        if not terminal:
            return proc.stderr
        os.close(slave)
        slave = None
        output = bytearray()
        while True:
            try:
                part = os.read(master, 4096)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                break
            if not part:
                break
            output.extend(part)
        return bytes(output).replace(b"\r\n", b"\n")
    finally:
        if master is not None:
            os.close(master)
        if slave is not None:
            os.close(slave)


for mode in ("compact", "normal", "learn"):
    flags = [f"--diag={mode}"]
    plain = run(flags, env)
    colored = run(flags, env, terminal=True)
    assert b"\x1b[1;31merror[W2001]" in colored
    assert b"\x1b[1;36m" in colored
    if mode != "compact":
        assert b"\x1b[1;33m" in colored
        assert b"\x1b[1;31m^^^^^" in colored
    if mode == "learn":
        assert b"\x1b[1;32mSUGGESTIONS" in colored
    assert b"\x1b" not in plain
    assert ansi.sub(b"", colored) == plain
    assert run(flags, dict(env, NO_COLOR="1"), terminal=True) == plain
    assert run(flags, dict(env, TERM="dumb"), terminal=True) == plain

for flags in (["--check", "--diag-json"], ["--check", "--errors-simple"]):
    assert b"\x1b" not in run(flags, env, terminal=True)

print("Diagnostic colors: OK")
