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
COLORS = {
    "Wind": "#79e1f2",
    "C": "#7ba7ff",
    "Rust": "#ffad86",
    "JavaScript": "#f5d379",
    "Python": "#ad9cff",
}


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
    run("gcc", "-O2", "-Wall", f"-I{src}", *sorted(src.glob("*.c")), "-o", tmp / "wind")
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
    width, height = 1280, 1080
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">']

    def rect(x, y, w, h, color, radius=0):
        svg.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" fill="{color}"/>')

    def text(x, y, label, size, color="#f1f5ff", weight=400, anchor="start"):
        svg.append(
            f'<text x="{x}" y="{y}" fill="{color}" font-size="{size}" '
            f'font-weight="{weight}" fill-opacity="1" text-anchor="{anchor}" '
            f'font-family="DejaVu Sans">{escape(label)}</text>'
        )

    def panel(top, title, rows, step):
        left, span, row_step = 276, 780, 46
        panel_height = 106 + len(rows) * row_step
        rect(68, top, 1144, panel_height, "#151e30", 18)
        text(96, top + 43, title, 23, weight=700)
        maximum = step * math.ceil(rows[-1][1] * 1.04 / step)
        start_y = top + 64
        axis_y = start_y + len(rows) * row_step
        for i in range(round(maximum / step) + 1):
            tick = i * step
            x = round(left + span * tick / maximum)
            rect(x, start_y, 1, axis_y - start_y, "#344056")
            text(x, axis_y + 25, f"{tick:g}".replace(".", ","), 14, "#a5b4cb", anchor="middle")
        text(1178, axis_y + 25, "мс", 14, "#a5b4cb", anchor="end")
        for i, (name, ms) in enumerate(rows):
            y = start_y + i * row_step
            color = COLORS[name]
            rect(96, y + 15, 8, 13, color, 2)
            text(117, y + 28, name, 19, weight=700)
            rect(left, y + 13, max(2, round(span * ms / maximum)), 16, color, 2)
            text(1178, y + 28, format_ru(ms, 2), 19, color, 700, "end")

    rect(0, 0, width, height, "#0d1422")
    rect(68, 55, 46, 4, COLORS["Wind"], 2)
    text(129, 63, "WIND LAB  /  01", 18, COLORS["Wind"], 700)
    text(68, 128, "Пять языков, одна сортировка", 50, weight=700)
    text(70, 176, "Сортировка вставками · 8 192 целых числа · меньше — быстрее", 23, "#a5b4cb")
    rows = sorted(result["median_ms"].items(), key=lambda row: row[1])
    fast_rows = [row for row in rows if row[0] != "Python"]
    step = 10 ** math.floor(math.log10(rows[-1][1])) / 4
    panel(218, "Все результаты", rows, step)
    step = 10 ** math.floor(math.log10(fast_rows[-1][1]))
    if fast_rows[-1][1] < 2 * step:
        step /= 2
    panel(574, "Увеличение · без Python", fast_rows, step)

    cpu = result["cpu"].replace("12th Gen ", "").replace("(R)", "").replace("(TM)", "")
    text(69, 925, f"{cpu}  /  {result['platform']}", 18, "#b0bdd3", 700)
    text(69, 960, f"Медиана {result['rounds']} замеров · 2 прогрева перед каждым · только время сортировки", 18, "#a5b4cb")
    text(69, 994, "Wind и C: GCC -O2 · Rust: opt-level=2 · JavaScript: Node.js · Python: CPython", 17, "#a5b4cb")
    svg.append("</svg>")
    source = tmp / "chart.svg"
    source.write_text("\n".join(svg), encoding="utf-8")
    run("magick", "-background", "#0d1422", source, "-strip", f"PNG24:{out}")


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
