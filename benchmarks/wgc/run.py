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


def measure(launcher, app, expected, profile=False):
    env = os.environ.copy()
    env.pop("WGC_STRESS", None)
    proc = subprocess.run([launcher, app], text=True, env=env, timeout=30, check=True,
                          capture_output=True)
    seconds, check, rss = proc.stdout.split()
    if int(check) != expected:
        raise RuntimeError(f"{app}: {proc.stdout}")
    if profile:
        prefix = "WGC_PROFILE "
        return json.loads(next(line[len(prefix):] for line in proc.stderr.splitlines()
                               if line.startswith(prefix)))
    return {"ms": float(seconds) * 1000, "rss_kib": int(rss)}


def benchmark(wind, rounds, work, baseline=None, profile=False):
    result = {"host": platform.platform(), "rounds": rounds, "workloads": {}}
    launcher = work / "rss"
    source = Path(__file__).with_name("rss.c")
    subprocess.run(["gcc", "-O2", str(source), "-o", launcher], check=True, timeout=30)
    rng = random.Random(42)
    for name, (source, expected) in WORKLOADS.items():
        path = work / "benchmark.wnd"
        path.write_text(source)
        modes = {"WGC": [wind, "--wgc"], "Boehm": [wind, "--boehm"]}
        if baseline:
            modes["WGC baseline"] = [baseline, "--wgc"]
        apps = {}
        profiled = None
        for mode, command in modes.items():
            subprocess.run([*command, path], cwd=work, check=True, timeout=30)
            apps[mode] = work / mode
            (work / "app").replace(apps[mode])
            if profile and mode == "WGC":
                runtime = Path(__file__).resolve().parents[2] / "runtime"
                profiled = work / "WGC-profile"
                subprocess.run(
                    ["gcc", "-O2", "-DWIND_GC_PROFILE", "-I", runtime, work / "output_ast.c",
                     runtime / "gc.c", runtime / "strings_gc.c", runtime / "collections_gc.c",
                     "-lm", "-o", profiled],
                    check=True, timeout=30,
                )

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
        if profiled:
            profiles = [measure(launcher, profiled, expected, profile=True) for _ in range(rounds)]
            result["workloads"][name]["WGC profile"] = {
                key: statistics.median(p[key] for p in profiles) for key in profiles[0]
            }
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wind", type=Path, default=Path("./wind"))
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--rounds", type=int, default=9)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    with tempfile.TemporaryDirectory(prefix="wind-gc-benchmark-") as tmp:
        result = benchmark(args.wind.resolve(), args.rounds, Path(tmp),
                           args.baseline.resolve() if args.baseline else None, args.profile)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
