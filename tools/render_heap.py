#!/usr/bin/env python3
"""Render a libmalloc heap snapshot (LM_HEAP_JSON / lm_dump_json) to the terminal and SVG.

  LM_HEAP_JSON=/tmp/heap.json LD_PRELOAD=build/libmalloc_shim.so ./prog
  python3 tools/render_heap.py /tmp/heap.json [--svg heap.svg]
  python3 tools/render_heap.py heap.json --check build/heap.expect   # used by `make json-test`
"""
import argparse, json, sys

SHADES = " .:-=+*#%@"


def human(n):
    for u in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or u == "GiB":
            return f"{n:.0f} {u}" if u == "B" else f"{n:.1f} {u}"
        n /= 1024


def text(d):
    print(f"mapped {human(d['mapped_bytes'])} (peak {human(d['peak_mapped_bytes'])}), "
          f"{d['used_blocks']} used blocks / {human(d['used_bytes'])}, "
          f"{d['free_blocks']} free blocks / {human(d['free_bytes'])}, "
          f"{d['large_mappings']} large mappings")
    print(f"largest free block {human(d['largest_free_block'])}; "
          f"external fragmentation {d['external_fragmentation']:.1%}")
    print("\nfree-list bins (blocks):")
    top = max((b["blocks"] for b in d["bins"]), default=1)
    for b in d["bins"]:
        hi = "inf" if not b["max"] else b["max"]
        print(f"  bin {b['bin']:3d} [{b['min']:>8}, {hi:>8}) {b['blocks']:6d} "
              f"{'#' * max(1, round(40 * b['blocks'] / top))}")
    print("\nlive block sizes (log2 buckets):")
    top = max((h["blocks"] for h in d["used_size_log2"]), default=1)
    for h in d["used_size_log2"]:
        print(f"  2^{h['log2']:<2d} {h['blocks']:6d} {'#' * max(1, round(40 * h['blocks'] / top))}")
    print("\nchunk occupancy (darker = more bytes in use):")
    for i, c in enumerate(d["chunks"]):
        row = "".join(SHADES[min(9, int(v * 9.999))] for v in c["occupancy"])
        print(f"  chunk {i:3d} |{row}|")


def svg(d, path):
    bw, rows = 8, len(d["chunks"])
    bins = d["bins"]
    top = max((b["blocks"] for b in bins), default=1)
    w, h = 64 * bw + 40, 60 + rows * 14 + 40 + len(bins) * 14 + 20
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" font-family="monospace" font-size="11">',
         f'<text x="10" y="16" font-size="13">heap: {human(d["used_bytes"])} used, {human(d["free_bytes"])} free, '
         f'external fragmentation {d["external_fragmentation"]:.1%}</text>',
         '<text x="10" y="40">chunk occupancy (each cell = 1/64 of a chunk)</text>']
    for i, c in enumerate(d["chunks"]):
        for k, v in enumerate(c["occupancy"]):
            g = int(235 - 200 * min(1.0, v))
            o.append(f'<rect x="{20 + k * bw}" y="{48 + i * 14}" width="{bw}" height="12" fill="rgb({g},{g},255)"/>')
    y = 48 + rows * 14 + 30
    o.append(f'<text x="10" y="{y - 8}">free-list bins (block count)</text>')
    for j, b in enumerate(bins):
        hi = "inf" if not b["max"] else b["max"]
        bar = max(2, 300 * b["blocks"] / top)
        o.append(f'<text x="10" y="{y + j * 14 + 10}">{b["min"]}-{hi}</text>')
        o.append(f'<rect x="150" y="{y + j * 14}" width="{bar:.0f}" height="11" fill="#c0504d"/>')
        o.append(f'<text x="{156 + bar:.0f}" y="{y + j * 14 + 10}">{b["blocks"]}</text>')
    o.append("</svg>")
    open(path, "w").write("\n".join(o))


def check(d, expect):
    used, free, large, used_bytes = map(int, open(expect).read().split())
    assert d["used_blocks"] == used, (d["used_blocks"], used)
    assert d["free_blocks"] == free, (d["free_blocks"], free)
    assert d["large_mappings"] == large
    assert d["used_bytes"] == used_bytes
    assert sum(b["blocks"] for b in d["bins"]) == free, "bins must account for every free block"
    assert sum(h["blocks"] for h in d["used_size_log2"]) == used
    assert all(len(c["occupancy"]) == 64 and all(0 <= v <= 1.0001 for v in c["occupancy"]) for c in d["chunks"])
    assert 0 <= d["external_fragmentation"] <= 1
    print("json snapshot consistent with lm_get_stats()")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("json")
    ap.add_argument("--svg")
    ap.add_argument("--check")
    a = ap.parse_args()
    d = json.load(open(a.json))
    if a.check:
        check(d, a.check)
        sys.exit(0)
    text(d)
    if a.svg:
        svg(d, a.svg)
        print(f"\nwrote {a.svg}")
