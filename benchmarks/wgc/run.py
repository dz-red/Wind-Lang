import argparse
import json
import os
import platform
import random
import statistics
import subprocess
import tempfile
from pathlib import Path

WORKLOADS = {
    "temporary": (
        """
int.total = 0
frac.start = time.clock()
for i in 0..200000
    str.value = "value ${i}"
    total += value.len
end
terminal.paste -> "${time.clock() - start} ${total}"
""",
        sum(len(f"value {i}") for i in range(200_000)),
    ),
    "retained": (
        """
int.total = 0
str.list.values = []
frac.start = time.clock()
for i in 0..80000
    values.add("value ${i}")
end
loop value in values
    total += value.len
end
terminal.paste -> "${time.clock() - start} ${total}"
""",
        sum(len(f"value {i}") for i in range(80_000)),
    ),
}


def measure(launcher, app, expected):
    env = os.environ.copy()
    env.pop("WGC_STRESS", None)
    output = subprocess.check_output([launcher, app], text=True, env=env, timeout=30)
    seconds, check, rss = output.split()
    if int(check) != expected:
        raise RuntimeError(f"{app}: {output}")
    return {"ms": float(seconds) * 1000, "rss_kib": int(rss)}


def benchmark(wind, rounds, work):
    result = {"host": platform.platform(), "rounds": rounds, "workloads": {}}
    launcher = work / "rss"
    source = Path(__file__).with_name("rss.c")
    subprocess.run(["gcc", "-O2", str(source), "-o", launcher], check=True, timeout=30)
    rng = random.Random(42)
    for name, (source, expected) in WORKLOADS.items():
        path = work / "benchmark.wnd"
        path.write_text(source)
        modes = {"WGC": ["--wgc"], "Boehm": ["--boehm"]}
        apps = {}
        for mode, flags in modes.items():
            subprocess.run([wind, *flags, path], cwd=work, check=True, timeout=30)
            apps[mode] = work / mode
            (work / "app").replace(apps[mode])

        order = list(apps)
        samples = {mode: [] for mode in order}
        for app in apps.values():
            measure(launcher, app, expected)
        for _ in range(rounds):
            rng.shuffle(order)
            for mode in order:
                samples[mode].append(measure(launcher, apps[mode], expected))
        result["workloads"][name] = {
            mode: {
                "median_ms": statistics.median(s["ms"] for s in runs),
                "median_rss_kib": statistics.median(s["rss_kib"] for s in runs),
                "samples": runs,
            }
            for mode, runs in samples.items()
        }
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wind", type=Path, default=Path("./wind"))
    parser.add_argument("--rounds", type=int, default=9)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    with tempfile.TemporaryDirectory(prefix="wind-gc-benchmark-") as tmp:
        result = benchmark(args.wind.resolve(), args.rounds, Path(tmp))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
