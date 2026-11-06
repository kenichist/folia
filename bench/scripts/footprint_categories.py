#!/usr/bin/env python3
"""Dirty MB per `footprint -p` category at each checkpoint of a run:
    python3 bench/scripts/footprint_categories.py bench/results/pdf-cache-2026-11-06/pdf-cache64"""
import re, sys, glob, os
U = {'B': 1, 'KB': 1024, 'MB': 1024**2, 'GB': 1024**3}
def parse(f):
    out = {}
    for line in open(f):
        m = re.match(r'\s*([\d.]+)\s*(B|KB|MB|GB)\s+([\d.]+)\s*(B|KB|MB|GB)\s+([\d.]+)\s*(B|KB|MB|GB)\s+(\d+)\s+(.*)$', line)
        if m:
            out[m.group(8).strip()] = float(m.group(1)) * U[m.group(2)] / 1e6
    return out
order = ['before-open', 'plateau', 'after-close', 'scanned-after-close', 'vector-after-close', 'transparency-after-close', 'after-cycles', 'after-release']
pre = sys.argv[1]
snaps = {l: parse(f) for l in order for f in [f'{pre}.footprint-{l}.txt'] if os.path.exists(f)}
labels = [l for l in order if l in snaps]
cats = ['TOTAL', 'untagged (VM_ALLOCATE)', 'IOSurface', 'MALLOC_LARGE', 'MALLOC_SMALL', 'MALLOC_TINY', 'Owned physical footprint (unmapped)']
print('| dirty MB | ' + ' | '.join(labels) + ' |')
print('|---|' + '---:|' * len(labels))
for c in cats:
    print(f'| {c} | ' + ' | '.join(f"{snaps[l].get(c, 0):.1f}" for l in labels) + ' |')
print('| TOTAL − IOSurface | ' + ' | '.join(f"{snaps[l].get('TOTAL', 0) - snaps[l].get('IOSurface', 0):.1f}" for l in labels) + ' |')
