"""Build, time, check and plot the five insertion-sort implementations."""

import argparse
import json
import math
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys
import tempfile
from xml.sax.saxutils import escape

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def run(*command, **kwargs):
    return subprocess.check_output(command, text=True, timeout=60, **kwargs).strip()


def expected_digest():
    values = []
    seed = 42
    for _ in range(8192):
        seed = (seed * 25173 + 13849) % 65536
        values.append(seed)
    checksum = 0
    for value in sorted(values):
        checksum = (checksum * 31 + value) % 65536
    return checksum


def machine():
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    return platform.processor() or platform.machine()


def benchmark(tmp, node, rounds):
    src = ROOT / "src"
    run("gcc", "-O2", "-Wall", f"-I{src}", f'-DWIND_RUNTIME_DIR="{ROOT / "runtime"}"',
        *sorted(src.glob("*.c")), "-o", tmp / "wind")
    run(tmp / "wind", HERE / "sort.wnd", cwd=tmp)
    run("gcc", "-O2", HERE / "sort.c", "-o", tmp / "sort-c")
    run("rustc", "-C", "opt-level=2", HERE / "sort.rs", "-o", tmp / "sort-rust")
    programs = {
        "Wind": (tmp / "app",),
        "C": (tmp / "sort-c",),
        "Rust": (tmp / "sort-rust",),
        "JavaScript": (node, HERE / "sort.mjs"),
        "Python": (sys.executable, HERE / "sort.py"),
    }
    checksum = expected_digest()
    rng = random.Random(42)
    languages = list(programs)
    samples = {language: [] for language in languages}
    for round_number in range(1, rounds + 1):
        rng.shuffle(languages)
        for language in languages:
            elapsed, digest = run(*programs[language]).split()
            ms = float(elapsed)
            if int(digest) != checksum or not math.isfinite(ms) or ms <= 0:
                raise ValueError(f"{language}: invalid measurement ({elapsed}, {digest})")
            samples[language].append(ms)
        print(f"Round {round_number}/{rounds}", flush=True)

    return {
        "algorithm": "insertion sort",
        "items": 8192,
        "seed": 42,
        "warmup_sorts_per_process": 2,
        "rounds": rounds,
        "checksum": checksum,
        "cpu": machine(),
        "platform": f"{platform.system()} {platform.machine()}",
        "versions": {
            "gcc": run("gcc", "--version").splitlines()[0],
            "rustc": run("rustc", "--version"),
            "python": platform.python_version(),
            "node": run(node, "--version"),
        },
        "samples_ms": samples,
        "median_ms": {name: statistics.median(times) for name, times in samples.items()},
    }


def format_ru(value, places=0):
    return f"{value:.{places}f}".replace(".", ",")


def render(result, tmp, out):
    width, height = 1080, 730
    paper, ink, muted = "#faf9f5", "#242723", "#525952"
    rule, bar, accent = "#d6d8d0", "#767e77", "#234f9c"
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">']

    def rect(x, y, w, h, color):
        svg.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{color}"/>')

    def text(x, y, label, size, color=ink, weight=400, anchor="start"):
        svg.append(
            f'<text x="{x}" y="{y}" fill="{color}" font-size="{size}" '
            f'font-weight="{weight}" text-anchor="{anchor}" '
            f'font-family="DejaVu Sans">{escape(label)}</text>'
        )

    def panel(top, title, rows, step):
        left, span, row_step = 212, 660, 34
        text(56, top, title, 20, weight=700)
        text(1024, top, "Время, мс", 15, muted, anchor="end")
        maximum = step * math.ceil(rows[-1][1] * 1.04 / step)
        start_y = top + 18
        axis_y = start_y + len(rows) * row_step
        for i in range(round(maximum / step) + 1):
            tick = i * step
            x = round(left + span * tick / maximum)
            rect(x, start_y, 1, axis_y - start_y, rule)
            text(x, axis_y + 21, f"{tick:g}".replace(".", ","), 13, muted, anchor="middle")
        for i, (name, ms) in enumerate(rows):
            y = start_y + i * row_step
            highlight = name == "Wind"
            color = accent if highlight else ink
            weight = 700 if highlight else 400
            text(56, y + 23, name, 18, color, weight)
            rect(left, y + 10, round(span * ms / maximum, 2), 14, accent if highlight else bar)
            text(1024, y + 23, format_ru(ms, 2), 18, color, weight, "end")

    rect(0, 0, width, height, paper)
    text(56, 79, "Сортировка вставками", 42, weight=700)
    text(56, 112, "8 192 целых числа, пять реализаций. Меньше — быстрее.", 20, muted)
    rect(56, 134, 968, 1, rule)
    rows = sorted(result["median_ms"].items(), key=lambda row: row[1])
    fast_rows = [row for row in rows if row[0] != "Python"]
    step = 10 ** math.floor(math.log10(rows[-1][1])) / 4
    panel(170, "Все пять языков", rows, step)
    step = 10 ** math.floor(math.log10(fast_rows[-1][1]))
    if fast_rows[-1][1] < 2 * step:
        step /= 2
    panel(434, "Крупнее: четыре результата без Python", fast_rows, step)

    cpu = result["cpu"].replace("12th Gen ", "").replace("(R)", "").replace("(TM)", "")
    rect(56, 644, 968, 1, rule)
    text(56, 671, f"{cpu} / {result['platform']} · Медиана {result['rounds']} замеров, без запуска и прогрева", 15, muted)
    text(56, 697, "Wind и C: GCC -O2 · Rust: opt-level=2 · JavaScript: Node.js · Python: CPython", 15, muted)
    svg.append("</svg>")
    source = tmp / "chart.svg"
    source.write_text("\n".join(svg), encoding="utf-8")
    run("magick", "-background", paper, source, "-strip", f"PNG24:{out}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node", default=os.environ.get("NODE", "node"))
    parser.add_argument("--rounds", type=int, default=9)
    parser.add_argument("--out", type=Path, default=HERE)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="wind-sort-") as work:
        tmp = Path(work)
        result = benchmark(tmp, args.node, args.rounds)
        render(result, tmp, args.out / "results.png")
    report = json.dumps(result, indent=2) + "\n"
    (args.out / "results.json").write_text(report, encoding="utf-8")
    print(report, end="")


if __name__ == "__main__":
    main()
