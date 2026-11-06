#!/usr/bin/env python3
"""Measure ImageIO decode cost per format and output size, and check the
MediaService cost model against it (docs/BENCHMARKS.md, "Image decode cost").

    python3 -I bench/scripts/mediaprobe_matrix.py --probe build/tools/folia_mediaprobe \
        --media <fixture dir> [--out results.json] [--reps 3]

The fixture dir is written by `folia_mediaprobe --make <dir>`. Each point runs
in a fresh process; the worst peak of --reps runs is compared with the
estimate. The model below must match MediaService::costFor/estimateFor.
"""
import argparse, json, os, subprocess

# (intermediate factor, output factor, fixed MiB, max power-of-two reduction)
MODEL = {
    "image/jpeg": (1.5, 2.0, 6, 3),
    "image/png": (1.9, 2.1, 32, 3),
    "image/heic": (1.0, 2.0, 8, 3),
    "image/webp": (1.2, 2.1, 4, 0),
}
DEFAULT = (1.2, 2.0, 12, 0)


def estimate(mime, nw, nh, ow, oh):
    a, b, f, kmax = MODEL.get(mime, DEFAULT)
    k = 0
    while k < kmax and ow > 0 and (nw >> (k + 1)) >= ow:
        k += 1
    inter = 4.0 * max(1, nw >> k) * max(1, nh >> k)
    return inter * a + 4.0 * ow * oh * b + f * 2**20


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True)
    ap.add_argument("--media", required=True)
    ap.add_argument("--out", default="")
    ap.add_argument("--reps", type=int, default=3)
    a = ap.parse_args()
    files = [("jpeg", "photo_00.jpg"), ("heic", "photo.heic"), ("webp", "photo.webp"),
             ("png_screenshot", "screenshot_0.png"), ("png_huge", "huge.png"), ("png_tall", "tall.png")]
    widths = [400, 800, 1400, 1520, 2800, 0]  # 0: native size
    rows = []
    for label, name in files:
        path = os.path.join(a.media, name)
        if not os.path.exists(path):
            continue
        for w in widths:
            runs = []
            for _ in range(a.reps):
                # max pixel size = the long side of an output `w` wide
                probe = json.loads(subprocess.run([a.probe, path, "1"], capture_output=True, text=True).stdout)
                nw, nh = probe["native_w"], probe["native_h"]
                ow = nw if w == 0 or w >= nw else w
                long_side = max(ow, round(ow * nh / nw))
                d = json.loads(subprocess.run([a.probe, path, str(long_side)], capture_output=True, text=True).stdout)
                runs.append(d)
            d = max(runs, key=lambda r: r["peak_delta_bytes"])
            est = estimate(d["mime"], d["native_w"], d["native_h"], d["out_w"], d["out_h"])
            rows.append({
                "format": label, "mime": d["mime"], "native": "%dx%d" % (d["native_w"], d["native_h"]),
                "output": "%dx%d" % (d["out_w"], d["out_h"]), "out_bytes": d["out_bytes"],
                "peak_bytes": d["peak_delta_bytes"], "peak_over_output": d["peak_over_output"],
                "estimate_bytes": est, "estimate_over_peak": est / d["peak_delta_bytes"] if d["peak_delta_bytes"] > 0 else 0,
                "decode_ms_median": sorted(r["decode_us"] for r in runs)[len(runs) // 2] / 1000.0,
                "cpu_ms_median": sorted(r["cpu_us"] for r in runs)[len(runs) // 2] / 1000.0,
                "retained_beyond_output_bytes": d["footprint_after"] - d["footprint_before"] - d["out_bytes"],
            })
    print("| Format | Native | Output | Peak (MB) | Peak ÷ output | Estimate (MB) | Estimate ÷ peak | Decode ms | Thread CPU ms |")
    print("|---|---|---|---:|---:|---:|---:|---:|---:|")
    for r in rows:
        print("| %s | %s | %s | %.1f | %.2f | %.1f | %.2f | %.0f | %.0f |" % (
            r["format"], r["native"], r["output"], r["peak_bytes"] / 2**20, r["peak_over_output"],
            r["estimate_bytes"] / 2**20, r["estimate_over_peak"], r["decode_ms_median"], r["cpu_ms_median"]))
    under = [r for r in rows if r["estimate_over_peak"] < 1.0]
    print("\nestimate below peak in %d of %d points" % (len(under), len(rows)))
    if a.out:
        with open(a.out, "w") as f:
            json.dump(rows, f, indent=1)


if __name__ == "__main__":
    main()
