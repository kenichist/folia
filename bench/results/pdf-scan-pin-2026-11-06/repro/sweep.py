import re, subprocess, sys, os
pdf, cycles, tile = sys.argv[1], sys.argv[2], sys.argv[3]
widths = [int(x) for x in sys.argv[4].split(',')]
for w in widths:
    out = subprocess.run(['./cgprobe', pdf, str(w), cycles, tile], capture_output=True, text=True).stdout
    f = [float(a) for a in re.findall(r'footprint=([\d.]+)MB', out)][1:]
    cpu = float(re.search(r'cpu_ms_per_cycle=([\d.]+)', out).group(1))
    g = (f[-1] - f[2]) / (len(f) - 3)
    print(f"  w={w:5d} growth c3->c{len(f)} {g:6.2f} MB/cycle  cpu {cpu:6.1f} ms/page" + ("  <-- pinned" if g > 1 else ""))
