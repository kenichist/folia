import json,sys,os
def p(xs,q):
  xs=sorted(xs);k=(len(xs)-1)*q/100;f=int(k);c=min(f+1,len(xs)-1);return xs[f]+(xs[c]-xs[f])*(k-f)
for f in sorted(sys.argv[1:]):
  j=json.load(open(f));m=j['metrics']
  s=m['input_to_paint_us']['samples']; e=m['editor_paint_us']['samples']; fl=m['input_to_flush_us']['samples']
  print('%-24s wall %5.1fs paints %4d  in->paint p50 %5.0f p95 %6.0f | paint p50 %4.0f p95 %5.0f | flush p50 %5.0f p95 %6.0f  mlc=%s clip=%s' % (
    os.path.basename(f), j['typing_wall_ms']/1000, len(e), p(s,50), p(s,95), p(e,50), p(e,95), p(fl,50), p(fl,95),
    j['env'].get('malloc_large_cache'), j.get('editor_painter_clip')))
