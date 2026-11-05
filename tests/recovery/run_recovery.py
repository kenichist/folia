#!/usr/bin/env python3
"""Crash / close / disk-full recovery tests against the real Folia binary.

Each case runs the app in bench mode on a fresh copy of a seeded workspace,
lets it type and save, interrupts it in a specific way, then reopens the
database directly and checks it against what the app had logged (fsync'd)
right before each save snapshot.

Usage: run_recovery.py --app build/src/Folia.app/Contents/MacOS/Folia --out results.json
"""
import argparse, json, os, shutil, signal, sqlite3, subprocess, sys, tempfile, time

OBJ = "￼"


def reconstruct(db, page):
    """Rebuild the editor's plain text from stored blocks (mirror of PageSession::load)."""
    con = sqlite3.connect(db)
    rev = con.execute("select rev from pages where id=?", (page,)).fetchone()[0]
    lines = []
    for kind, lang, text, target in con.execute(
            "select kind, lang, text, target from blocks where page_id=? order by ord", (page,)):
        if kind == 0:
            lines.extend(text.split("\n"))
        elif kind == 1:
            lines.append("```" + lang); lines.extend(text.split("\n")); lines.append("```")
        elif kind == 2:
            if lang == "single":
                lines.append("$$" + text + "$$")
            else:
                lines.append("$$"); lines.extend(text.split("\n")); lines.append("$$")
        elif kind == 3:
            lines.append(OBJ)
    integrity = con.execute("pragma integrity_check").fetchone()[0]
    con.close()
    return rev, "\n".join(lines) if lines else "", integrity


def read_log(path):
    out = []
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        out.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass  # torn final line from a crash
    return out


def seed(app, home):
    subprocess.run([app, "--home", home, "--no-watch", "--bench", "snapshot",
                    "--bench-arg", "png=" + os.path.join(home, "seed.png"), "--bench-arg", "wait=300"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)


def run_case(app, base, name, args, env_extra=None, kill_after=None, page=2, flags=()):
    home = os.path.join(tempfile.mkdtemp(prefix="folia-rec-"), "home")
    shutil.copytree(base, home)
    for f in os.listdir(home):
        if f.endswith(".lock"):
            os.remove(os.path.join(home, f))
    log = os.path.join(home, "snapshots.jsonl")
    out = os.path.join(home, "result.json")
    cmd = [app, "--home", home, "--no-watch", "--bench", "recovery", "--bench-out", out,
           "--bench-arg", "page=%d" % page, "--bench-arg", "log=" + log] + list(flags)
    for k, v in args.items():
        cmd += ["--bench-arg", "%s=%s" % (k, v)]
    env = dict(os.environ)
    env.update(env_extra or {})
    t0 = time.time()
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if kill_after is not None:
        # Kill hard once enough snapshots exist (mid-burst, between autosaves).
        deadline = time.time() + 60
        while time.time() < deadline and len(read_log(log)) < kill_after and p.poll() is None:
            time.sleep(0.02)
        time.sleep(0.11)
        p.send_signal(signal.SIGKILL)
    try:
        _, err = p.communicate(timeout=180)
    except subprocess.TimeoutExpired:
        p.kill(); _, err = p.communicate()
    rc = p.returncode
    logs = read_log(log)
    rev, text, integrity = reconstruct(os.path.join(home, "workspace.db"), page)
    result = {}
    if os.path.exists(out):
        with open(out) as f:
            result = json.load(f)
    return dict(name=name, rc=rc, seconds=round(time.time() - t0, 2), db_rev=rev, integrity=integrity,
                logs=logs, text=text, result=result, stderr=err.decode(errors="replace")[-400:])


def check(case, expect):
    logs = case["logs"]
    by_rev = {int(l["rev"]): l for l in logs}
    ok = case["integrity"] == "ok"
    reasons = [] if ok else ["integrity_check: " + case["integrity"]]
    match = by_rev.get(int(case["db_rev"]))
    if not match:
        reasons.append("db rev %s matches no logged snapshot" % case["db_rev"])
        ok = False
    elif match["text"] != case["text"]:
        reasons.append("db text differs from snapshot rev %s" % case["db_rev"])
        ok = False
    snaps = [l for l in logs if l["event"] in ("snapshot", "before_close", "final")]
    if expect == "previous" and len(snaps) >= 2 and int(case["db_rev"]) != int(snaps[-2]["rev"]):
        reasons.append("expected rev of previous snapshot %s, got %s" % (snaps[-2]["rev"], case["db_rev"]))
        ok = False
    if expect == "last" and snaps and int(case["db_rev"]) != int(snaps[-1]["rev"]):
        reasons.append("expected rev of last snapshot %s, got %s" % (snaps[-1]["rev"], case["db_rev"]))
        ok = False
    if expect == "any_recent" and len(snaps) >= 2 and int(case["db_rev"]) < int(snaps[-2]["rev"]):
        reasons.append("lost more than the in-flight snapshot")
        ok = False
    return ok, reasons


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    base = os.path.join(tempfile.mkdtemp(prefix="folia-seed-"), "home")
    os.makedirs(base)
    seed(a.app, base)
    cases = []
    c = run_case(a.app, base, "crash_before_commit", {"mode": "log", "rounds": "40"},
                 {"FOLIA_CRASH_POINT": "before-commit", "FOLIA_CRASH_AFTER": "8"})
    cases.append((c, "previous"))
    c = run_case(a.app, base, "crash_after_commit", {"mode": "log", "rounds": "40"},
                 {"FOLIA_CRASH_POINT": "after-commit", "FOLIA_CRASH_AFTER": "8"})
    cases.append((c, "last"))
    c = run_case(a.app, base, "sigkill_mid_typing", {"mode": "log", "rounds": "200", "every_ms": "150"}, kill_after=12)
    cases.append((c, "any_recent"))
    c = run_case(a.app, base, "normal_close_flushes", {"mode": "quit", "n": "250"})
    cases.append((c, "last"))
    c = run_case(a.app, base, "disk_full_then_space", {"mode": "diskfull", "rounds": "14", "fail_after": "4"}, None,
                 flags=["--fault-vfs"])
    cases.append((c, "last"))
    report = []
    all_ok = True
    for case, expect in cases:
        ok, reasons = check(case, expect)
        if case["name"] == "disk_full_then_space":
            r = case["result"]
            for k in ("disk_full_surfaced", "recovered_after_space_freed", "editor_text_kept"):
                if not r.get(k):
                    ok = False
                    reasons.append(k + " is false")
        all_ok &= ok
        report.append(dict(name=case["name"], ok=ok, reasons=reasons, exit_code=case["rc"], seconds=case["seconds"],
                           db_rev=case["db_rev"], integrity=case["integrity"], snapshots=len(case["logs"]),
                           last_snapshot_rev=case["logs"][-1]["rev"] if case["logs"] else None,
                           text_chars=len(case["text"]),
                           extra={k: v for k, v in case["result"].items() if k in (
                               "disk_full_surfaced", "disk_full_status_text", "recovered_after_space_freed",
                               "injected_failures", "final_rev", "editor_text_kept")},
                           stderr_tail=case["stderr"].strip().splitlines()[-2:] if case["stderr"].strip() else []))
        print(("PASS " if ok else "FAIL ") + case["name"], "; ".join(reasons))
    with open(a.out, "w") as f:
        json.dump(dict(all_ok=all_ok, cases=report), f, indent=2, ensure_ascii=False)
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
