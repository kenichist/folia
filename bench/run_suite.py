#!/usr/bin/env python3
"""Run the Folia acceptance benchmark suite on this machine.

    python3 -I bench/run_suite.py --app build/src/Folia.app/Contents/MacOS/Folia \
        --fixturegen build/tools/folia_fixturegen --out bench/results/<name> [--quick]

Every run gets a fresh copy of the fixture workspace (so create/typing runs do
not accumulate), its own data folder and the preset under test. Raw JSON per
run is kept; bench/scripts/analyze.py turns the folder into a report.
"""
import argparse, json, os, platform, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sh(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout.strip()
    except Exception as e:  # noqa: BLE001
        return "unavailable (%s)" % e


def environment():
    return {
        "host": platform.node(),
        "machine": sh(["sysctl", "-n", "hw.model"]),
        "cpu": sh(["sysctl", "-n", "machdep.cpu.brand_string"]),
        "ram_bytes": sh(["sysctl", "-n", "hw.memsize"]),
        "os": sh(["sw_vers", "-productVersion"]) + " (" + sh(["sw_vers", "-buildVersion"]) + ")",
        "power": sh(["pmset", "-g", "batt"]).splitlines()[:2],
        "low_power_mode": sh(["pmset", "-g"]).count("lowpowermode         1") > 0,
        "thermal": sh(["pmset", "-g", "therm"]),
        "display": "built-in Retina 60 Hz (2560x1600 panel)",
        "git_commit": sh(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"]) or "uncommitted working tree",
        "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--fixturegen", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", default="42")
    ap.add_argument("--quick", action="store_true", help="fewer runs/iterations (smoke)")
    ap.add_argument("--only", default="", help="comma-separated scenario labels")
    a = ap.parse_args()
    app = os.path.abspath(a.app)
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    fixdir = os.path.join(out, "fixture")
    os.makedirs(fixdir, exist_ok=True)
    fixture_db = os.path.join(fixdir, "workspace.db")
    manifest = os.path.join(fixdir, "manifest.json")
    if not os.path.exists(fixture_db):
        t = time.time()
        subprocess.run([a.fixturegen, "--out", fixture_db, "--seed", a.seed, "--manifest", manifest], check=True)
        print("fixture generated in %.1fs" % (time.time() - t), flush=True)
    with open(manifest) as f:
        man = json.load(f)
    # Image fixtures for the `images` scenario, written by the decode-cost
    # probe next to the fixture generator (out of process, so the scenario's
    # footprint baseline does not carry the encoder's memory).
    media_dir = os.path.join(fixdir, "media")
    probe = os.path.join(os.path.dirname(os.path.abspath(a.fixturegen)), "folia_mediaprobe")
    pdf_dir = os.path.join(fixdir, "pdf")
    if os.path.exists(probe):
        subprocess.run([probe, "--make", media_dir, "6" if a.quick else "24"], check=True,
                       stdout=subprocess.DEVNULL)
        # PDF fixtures for the `pdf` scenario (CoreGraphics; macOS only).
        subprocess.run([probe, "--make-pdf", pdf_dir], check=False, stdout=subprocess.DEVNULL)
    sample = man.get("sample_pages", [])
    q = a.quick
    R = (lambda n: 1) if q else (lambda n: n)
    N = (lambda n, small: small) if q else (lambda n, small: n)
    runs = [
        # label, scenario, args, repetitions, extra flags
        ("launch", "launch", {"start_page": man.get("ordinary_page")}, R(5), []),
        ("open", "open", {"n": N(120, 30)}, R(3), []),
        ("create", "create", {"n": N(120, 20)}, R(3), []),
        ("expand", "expand", {"n": N(100, 20)}, R(3), []),
        ("typing", "typing", {"doc": "ordinary", "n": N(1000, 200)}, R(3), []),
        ("typing_caret_top", "typing", {"doc": "ordinary", "n": N(1000, 200), "caret": "top"}, R(2), []),
        ("typing_keyboard_preset", "typing", {"doc": "ordinary", "n": N(1000, 200)}, R(3), ["--preset", "keyboard"]),
        ("typing_after_invalid_theme", "typing", {"doc": "ordinary", "n": N(1000, 200), "recover": 1}, R(2), []),
        ("typing_during_bg_render", "typing", {"doc": "ordinary", "n": N(1000, 200), "bgrender": 1}, R(2), []),
        ("typing_stress_note", "typing", {"doc": "stress", "n": N(500, 100)}, R(2), []),
        ("typing_huge_code_block", "typing", {"doc": "code", "n": N(500, 100)}, R(2), []),
        ("scroll", "scroll", {"frames": N(1200, 300)}, R(3), []),
        ("stress", "stress", {"n": N(20, 5)}, R(2), []),
        ("presentation", "presentation", {"n": N(100, 20)}, R(3), []),
        ("idle_default", "idle", {"seconds": N(60, 10), "page": man.get("ordinary_page")}, R(2), []),
        ("idle_keyboard_preset", "idle", {"seconds": N(60, 10), "page": man.get("ordinary_page")}, R(2), ["--preset", "keyboard"]),
        ("idle_no_math", "idle", {"seconds": N(60, 10), "page": man.get("ordinary_page")}, R(1), ["--no-math"]),
        ("idle_editor_only_shell", "idle", {"seconds": N(60, 10), "page": man.get("ordinary_page")}, R(1),
         ["--no-math", "--shell", os.path.join(ROOT, "presets/examples/minimal/shell.lua")]),
        ("idle_stress_note", "idle", {"seconds": N(60, 10), "page": man.get("stress_page")}, R(1), []),
        ("wake", "wake", {"gap_ms": 12000, "rounds": N(4, 2)}, R(1), []),
        ("navigate", "navigate", {"batches": 5, "per_batch": N(100, 20)}, R(1), []),
        ("images", "images", {"photos": N(24, 6), "idle_seconds": N(20, 5), "media_dir": media_dir}, R(1), []),
        ("pdf", "pdf", {"pdf_dir": pdf_dir, "idle_seconds": N(20, 5), "zoom_rounds": N(6, 2)}, R(1), []),
        ("integration", "integration", {"ordinary_page": sample[0] if sample else man.get("ordinary_page"),
                                         "math_page": man.get("ordinary_page")}, R(1), []),
    ]
    only = set(filter(None, a.only.split(",")))
    env = environment()
    with open(os.path.join(out, "environment.json"), "w") as f:
        json.dump(dict(env, fixture=man, app=app), f, indent=2)
    print(json.dumps(env, indent=1), flush=True)
    for label, scenario, args, reps, flags in runs:
        if only and label not in only:
            continue
        for rep in range(reps):
            home = os.path.join(out, "homes", "%s-%d" % (label, rep))
            shutil.rmtree(home, ignore_errors=True)
            os.makedirs(home)
            shutil.copy(fixture_db, os.path.join(home, "workspace.db"))
            res = os.path.join(out, "%s.%d.json" % (label, rep))
            # Live reload stays on for integration (it checks reload-during-IME).
            watch = [] if scenario == "integration" else ["--no-watch"]
            cmd = [app, "--home", home] + watch + ["--bench", scenario, "--bench-out", res,
                   "--bench-arg", "manifest=" + manifest] + flags
            for k, v in args.items():
                cmd += ["--bench-arg", "%s=%s" % (k, v)]
            t = time.time()
            p = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=1800)
            ok = p.returncode == 0 and os.path.exists(res)
            print("%-28s run %d: %s in %.1fs" % (label, rep, "ok" if ok else "FAILED rc=%s" % p.returncode,
                                                time.time() - t), flush=True)
            if not ok:
                with open(res + ".stderr.txt", "w") as f:
                    f.write(p.stderr.decode(errors="replace"))
            shutil.rmtree(home, ignore_errors=True)  # keep only results
    print("done; analyze with: python3 -I bench/scripts/analyze.py %s/*.json" % out)


if __name__ == "__main__":
    main()
