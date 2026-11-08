# lldb script: who creates / resets the QClipData objects whose spans get (re)allocated?
# command script import cliptrace.py ; then process launch. Dumps to $CLIPTRACE_OUT at exit.
import lldb, os, collections

events = []          # (kind, this, frames)
DEPTH = 18
started = [False]    # only record once the typing loop is running (NoteEditor paints seen)

def frames(thread, skip=0):
    out = []
    for i in range(skip, min(thread.GetNumFrames(), skip + DEPTH)):
        f = thread.GetFrameAtIndex(i)
        n = f.GetFunctionName() or f.GetSymbol().GetName() or hex(f.GetPC())
        out.append(n)
    return tuple(out)

def reg(frame, name):
    return frame.FindRegister(name).GetValueAsUnsigned()

def mk(kind, check_spans=False):
    def cb(frame, bp_loc, internal_dict):
        this = reg(frame, "x0")
        if check_spans:
            err = lldb.SBError()
            spans = frame.GetThread().GetProcess().ReadPointerFromMemory(this + 0x18, err)
            if spans != 0:
                return False
        events.append((kind, this, frames(frame.GetThread())))
        return False
    return cb

def __lldb_init_module(debugger, internal_dict):
    target = debugger.GetSelectedTarget()
    specs = [
        ("ctor", "QClipData::QClipData(int)", False),
        ("dtor", "QClipData::~QClipData()", False),
        ("setRect", "QClipData::setClipRect(QRect const&)", False),
        ("setRegion", "QClipData::setClipRegion(QRegion const&)", False),
        ("init", "QClipData::initialize()", True),
        ("sysState", "QRasterPaintEnginePrivate::systemStateChanged()", False),
    ]
    g = globals()
    for kind, name, chk in specs:
        bp = target.BreakpointCreateByName(name, "QtGui")
        fn = "cb_" + kind
        g[fn] = mk(kind, chk)
        bp.SetScriptCallbackFunction("cliptrace." + fn)
        bp.SetAutoContinue(False)
    debugger.HandleCommand("command script add -f cliptrace.dump cliptrace_dump")

def short(fs, n):
    return " <- ".join(x.split("(")[0] for x in fs[:n])

def dump(debugger, command, result, internal_dict):
    out = open(os.environ.get("CLIPTRACE_OUT", "/dev/stdout"), "w")
    counts = collections.Counter(k for k, _, _ in events)
    out.write("event counts: %s\n" % dict(counts))
    # For each init that allocates, find the last ctor/setRegion/setRect on that object.
    last = {}
    origin = collections.Counter()
    initsites = collections.Counter()
    for kind, this, fs in events:
        if kind in ("ctor", "setRect", "setRegion"):
            last[this] = (kind, fs)
        elif kind == "init":
            o = last.get(this, ("?", ()))
            origin[(o[0], short(o[1], 10))] += 1
            initsites[short(fs, 9)] += 1
    out.write("\n== allocating initialize(), by what last created/reset that clip ==\n")
    for (k, s), c in origin.most_common(15):
        out.write("%6d  %s: %s\n" % (c, k, s))
    out.write("\n== allocating initialize() call sites ==\n")
    for s, c in initsites.most_common(10):
        out.write("%6d  %s\n" % (c, s))
    for kind in ("ctor", "setRegion", "setRect", "sysState"):
        out.write("\n== %s stacks ==\n" % kind)
        cc = collections.Counter(short(fs, 12) for k, _, fs in events if k == kind)
        for s, c in cc.most_common(10):
            out.write("%6d  %s\n" % (c, s))
    out.close()
