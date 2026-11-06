#!/usr/bin/env python3
"""Measure CoreGraphics PDF render cost: whole page vs. tiles, per document
kind and output size (docs/BENCHMARKS.md §6, "PDF render cost").

    python3 -I bench/scripts/pdfprobe_matrix.py --probe build/tools/folia_mediaprobe \
        --pdfs <fixture dir> [--out results.json]

The fixture dir is written by `folia_mediaprobe --make-pdf <dir>`. Each point
runs in a fresh process (the peak is process-wide). For tiles, the tiles
covering one viewport (2200×1600 physical px, a 1100×800 pt window on a 2x
display) are rendered one after another, each into its own bitmap; the peak
reported is the worst single tile's. The reservation is the probe's
own MediaService::pdfEstimate for the largest bitmap of the point.
"""
import argparse, json, os, subprocess

DOCS = [("text", "long-200.pdf", 2), ("scanned", "scanned-24.pdf", 2),
        ("vector", "vector-16.pdf", 2), ("transparency", "transparency-16.pdf", 2)]
WIDTHS = [600, 1200, 2000, 3000, 4000, 6000]
MODES = [0, 512, 1024]
VIEW = (2200, 1600)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True)
    ap.add_argument("--pdfs", required=True)
    ap.add_argument("--out", default="")
    ap.add_argument("--widths", default=",".join(map(str, WIDTHS)))
    ap.add_argument("--modes", default=",".join(map(str, MODES)))
    a = ap.parse_args()
    widths = [int(x) for x in a.widths.split(",")]
    modes = [int(x) for x in a.modes.split(",")]
    rows = []
    for kind, name, page in DOCS:
        path = os.path.join(a.pdfs, name)
        for w in widths:
            for tile in modes:
                cmd = [a.probe, "--pdf", path, str(page), str(w), str(tile), str(VIEW[0]), str(VIEW[1])]
                out = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
                if out.returncode != 0:
                    print("FAILED", cmd, out.stderr)
                    continue
                r = json.loads(out.stdout)
                r["kind"] = kind
                r["estimate_over_peak"] = r["estimate_bytes"] / max(1, r["cold_peak_delta_bytes"])
                rows.append(r)
                print(f'{kind:13} w={w:5} tile={tile:5} n={r["bitmaps"]:3} '
                      f'cold={r["cold_ms"]:8.1f}ms warm={r["warm_ms"]:8.1f}ms '
                      f'peak={r["cold_peak_delta_bytes"]/2**20:6.1f}MiB/{r["warm_peak_delta_bytes"]/2**20:6.1f}MiB '
                      f'out={r["out_bytes"]/2**20:6.1f}MiB est/peak={r["estimate_over_peak"]:.2f} '
                      f'open={r["doc_open_retained_bytes"]/2**20:5.1f}MiB closed={r["after_close_retained_bytes"]/2**20:5.1f}MiB',
                      flush=True)
    if a.out:
        with open(a.out, "w") as f:
            json.dump(rows, f, indent=1)


if __name__ == "__main__":
    main()
