#!/usr/bin/env python3
"""Run the benchmark matrix and write results/bench.json.

Usage: python3 bench/run.py [--out results/bench.json]
Requires bench/build.sh to have been run.
"""
import argparse, json, os, platform, subprocess, sys, datetime

WORKLOADS = ["random", "realloc", "small", "bigrandom", "mt4"]
# (label, binary, impl arg). The baseline is single-threaded and was built for
# heaps <= 4 MiB, so it only runs the first three workloads.
IMPLS = [("baseline", "build/bin/bench_old", "lm"),
         ("current", "build/bin/bench_new", "lm"),
         ("system", "build/bin/bench_new", "sys")]
BASELINE_WORKLOADS = {"random", "realloc", "small"}


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()


def hardware():
    if platform.system() == "Darwin":
        return {"cpu": sh("sysctl -n machdep.cpu.brand_string"),
                "mem_bytes": int(sh("sysctl -n hw.memsize")),
                "cores": int(sh("sysctl -n hw.ncpu"))}
    return {"cpu": sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2").strip(),
            "mem_bytes": int(sh("awk '/MemTotal/{print $2*1024}' /proc/meminfo")),
            "cores": int(sh("nproc"))}


def markdown(out):
    by = {(x["workload"], x["impl"]): x for x in out["results"]}
    hw = out["hardware"]
    lines = [f"Hardware: {hw['cpu']}, {hw['cores']} cores, {hw['mem_bytes'] >> 30} GiB; {out['os']}; "
             f"{out['compiler']}; flags `{out['flags']}`", "",
             "| workload | current ops/s | system ops/s | system / current |", "|---|---|---|---|"]
    for wl in WORKLOADS:
        c, s = by.get((wl, "current")), by.get((wl, "system"))
        if not c or not s or "failed" in c or "failed" in s:
            continue
        a, b = c["ops_per_s_median"], s["ops_per_s_median"]
        lines.append(f"| {wl} | {a:,} | {b:,} | {b / a:.2f}x |")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="results/bench.json")
    ap.add_argument("--markdown", action="store_true", help="also print a current-vs-system table")
    args = ap.parse_args()
    out = {"date": datetime.date.today().isoformat(),
           "hardware": hardware(),
           "os": platform.platform(),
           "compiler": sh("cc --version | head -1"),
           "flags": os.environ.get("OPT", "-O2") + " -std=gnu11 -pthread",
           "notes": "median of 5 runs per cell (after one discarded warm-up); fragmentation = peak heap bytes "
                    "/ peak requested bytes (not reported for the system allocator)",
           "results": []}
    for wl in WORKLOADS:
        for label, binary, impl in IMPLS:
            if label == "baseline" and wl not in BASELINE_WORKLOADS:
                continue
            r = subprocess.run([binary, impl, wl], capture_output=True, text=True)
            if r.returncode != 0:
                row = {"impl": label, "workload": wl, "failed": r.stderr.strip()}
            else:
                row = json.loads(r.stdout)
                row["impl"] = label
            print(json.dumps(row), file=sys.stderr)
            out["results"].append(row)
    with open(args.out, "w") as f:
        json.dump(out, f, indent=2)
        f.write("\n")
    if args.markdown:
        print(markdown(out))


if __name__ == "__main__":
    main()
