#!/usr/bin/env python3
"""Phase gate for the Stellarium Android/Cardboard port.

Runs the project's mandated gate after every phase:
    BUILD -> LINT -> STATIC ANALYSIS -> UNIT TESTS -> REPORT

Thresholds enforced (per project spec):
    BUILD ERRORS = 0
    FATAL / ERROR-LEVEL LINT = 0
    NEW LINT WARNINGS = 0 (target)
    FAILED TESTS = 0

BASELINE-DIFF DESIGN (important)
    Upstream Stellarium is NOT clang-format clean under a current clang-format, and
    its .clang-tidy is unparseable by modern clang-tidy. So a naive "lint the tree"
    gate reports hundreds of pre-existing issues and is useless for judging OUR work.
    This gate therefore:
      * clang-format: compares violation count against the PRISTINE upstream version
        of each file (git show upstream/master:<path>). A file fails only if it is
        newly misformatted relative to its own upstream baseline.
      * clang-tidy: runs the port config (notes/clang-tidy-port.yaml) on changed files
        only, with the host build's compile_commands.json.
    Files this port ADDS must be fully clean (no baseline to excuse them).

Usage:
    python phase_gate.py --phase 1
    python phase_gate.py --phase 1 --skip-build
    python phase_gate.py --phase 1 --files src/core/Foo.cpp
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

# Paths derive from this file's location:  <PROJECT>/stellarium/android-port/phase_gate.py
# so the tool keeps working wherever the fork is checked out.
REPO       = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT       = os.path.dirname(REPO)
HOST_BUILD = os.path.join(ROOT, "build", "host")
AND_BUILD  = os.path.join(ROOT, "build", "android-arm64")
TIDY_CFG   = os.path.join(os.path.dirname(os.path.abspath(__file__)), "clang-tidy-port.yaml")
LLVM_BIN   = r"C:/Program Files/LLVM/bin"
CMAKE_BIN  = r"C:/Users/Raka/AppData/Local/Android/Sdk/cmake/3.22.1/bin"
QT_BIN     = r"D:/Qt/6.7.3/msvc2019_64/bin"
NINJA_DIR  = r"C:/Users/Raka/AppData/Local/Microsoft/WinGet/Links"

CLANG_FORMAT = os.path.join(LLVM_BIN, "clang-format.exe")
CLANG_TIDY   = os.path.join(LLVM_BIN, "clang-tidy.exe")
CTEST        = os.path.join(CMAKE_BIN, "ctest.exe")
CMAKE        = os.path.join(CMAKE_BIN, "cmake.exe")

SRC_EXT = (".cpp", ".hpp", ".h", ".c", ".cc", ".cxx")


def sh(cmd, cwd=None, timeout=3600, env=None, shell=False):
    e = dict(os.environ)
    e["PATH"] = os.pathsep.join([LLVM_BIN, CMAKE_BIN, QT_BIN, NINJA_DIR, e.get("PATH", "")])
    if env:
        e.update(env)
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                       timeout=timeout, shell=shell, env=e)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def upstream_has(path):
    rc, _ = sh(["git", "-C", REPO, "cat-file", "-e", f"upstream/master:{path}"])
    return rc == 0


def changed_files(explicit=None):
    if explicit:
        return [f for f in explicit if f.endswith(SRC_EXT)]
    files = []
    rc, out = sh(["git", "-C", REPO, "diff", "--name-only", "upstream/master...HEAD"])
    files += [f.strip() for f in out.splitlines() if f.strip()]
    rc, out = sh(["git", "-C", REPO, "status", "--porcelain"])
    for line in out.splitlines():
        p = line[3:].strip()
        if p:
            files.append(p)
    seen, uniq = set(), []
    for f in files:
        if f not in seen and f.endswith(SRC_EXT):
            seen.add(f)
            uniq.append(f)
    return uniq


def format_violations(path, content=None):
    """Count clang-format violations for a file (or given content)."""
    if content is None:
        rc, out = sh([CLANG_FORMAT, "--dry-run", "--Werror", path])
        if rc == 0:
            return 0
        return sum(1 for l in out.splitlines() if "-Wclang-format-violations" in l)
    with tempfile.NamedTemporaryFile("w", suffix=os.path.splitext(path)[1],
                                     delete=False, encoding="utf-8") as fh:
        fh.write(content)
        tmp = fh.name
    try:
        rc, out = sh([CLANG_FORMAT, "--dry-run", "--Werror", "--assume-filename", path, tmp])
        if rc == 0:
            return 0
        return sum(1 for l in out.splitlines() if "-Wclang-format-violations" in l)
    finally:
        os.unlink(tmp)


def step_build(build_dir, label):
    if not os.path.isdir(build_dir):
        return {"step": f"BUILD ({label})", "status": "SKIP", "detail": "build dir absent"}
    rc, out = sh([CMAKE, "--build", build_dir, "--parallel"], timeout=7200)
    errs = out.count("error:")
    detail = f"exit={rc}, compile 'error:' lines={errs}"
    status = "PASS" if rc == 0 else "FAIL"
    tail = "\n".join(out.strip().splitlines()[-15:])

    # The Android build's final step is androiddeployqt, which upstream invokes with
    # --release. That path needs a signing keystore (which this port does not have yet)
    # and makes Gradle pick the newest installed SDK platform, which the Qt-bundled AGP
    # cannot handle ("Failed to find Platform SDK with path: platforms;android-37").
    # Detect that specific case and report it accurately instead of as a code failure:
    # compilation itself succeeded.
    if rc != 0 and "androiddeployqt" in out and "Platform SDK with path: platforms;android-37" in out:
        detail += " — compilation OK; APK packaging step needs the debug path (see note)"
        tail += ("\n\n[gate] androiddeployqt --release cannot work yet: no signing keystore, "
                 "and it selects platforms;android-37 which the bundled AGP rejects.\n"
                 "[gate] Build the APK separately with:\n"
                 "        androiddeployqt --input android-stellarium-deployment-settings.json \\\n"
                 "          --output android-build --apk android-build/stellarium.apk \\\n"
                 "          --android-platform android-34 --debug\n"
                 "[gate] See android-port/README.md and the stellarium-android-port skill.")
    return {"step": f"BUILD ({label})", "status": status, "detail": detail, "log_tail": tail}


def step_format(files):
    """Baseline-diffed clang-format: fail only on NEW deviations."""
    if not files:
        return {"step": "LINT (clang-format, baseline-diff)", "status": "SKIP",
                "detail": "no changed source files vs upstream/master"}
    rows, regressions, new_files_bad = [], [], []
    for f in files:
        p = os.path.join(REPO, f)
        if not os.path.exists(p):
            continue
        now = format_violations(p)
        if upstream_has(f):
            rc, base = sh(["git", "-C", REPO, "show", f"upstream/master:{f}"])
            before = format_violations(f, base) if rc == 0 else 0
            delta = now - before
            rows.append(f"{f}: {before} -> {now}")
            if delta > 0:
                regressions.append(f"{f}: +{delta} new violation(s) ({before} -> {now})")
        else:
            rows.append(f"{f}: new file, {now} violation(s)")
            if now > 0:
                new_files_bad.append(f"{f}: {now} violation(s) in a NEW file")
    bad = regressions + new_files_bad
    return {"step": "LINT (clang-format, baseline-diff)",
            "status": "PASS" if not bad else "FAIL",
            "detail": f"{len(rows)} file(s); {len(bad)} newly misformatted",
            "per_file": rows[:25], "offenders": bad[:15]}


def step_tidy(files, build_dir=HOST_BUILD):
    cc = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(cc):
        return {"step": "STATIC ANALYSIS (clang-tidy)", "status": "SKIP",
                "detail": f"no compile_commands.json in {build_dir}"}
    if not files:
        return {"step": "STATIC ANALYSIS (clang-tidy)", "status": "SKIP",
                "detail": "no changed files (baseline-diff mode)"}
    if not os.path.exists(TIDY_CFG):
        return {"step": "STATIC ANALYSIS (clang-tidy)", "status": "SKIP",
                "detail": f"port config missing: {TIDY_CFG}"}
    errors, warnings = [], []
    for f in files:
        p = os.path.join(REPO, f)
        if not os.path.exists(p):
            continue
        rc, out = sh([CLANG_TIDY, f"--config-file={TIDY_CFG}", "-p", build_dir, p],
                     timeout=1200)
        if "error: unknown key" in out or "Error parsing" in out:
            return {"step": "STATIC ANALYSIS (clang-tidy)", "status": "FAIL",
                    "detail": "config parse error", "errors": [out.strip()[:300]]}
        for line in out.splitlines():
            if ": error:" in line:
                errors.append(line.strip())
            elif ": warning:" in line:
                warnings.append(line.strip())
    return {"step": "STATIC ANALYSIS (clang-tidy)",
            "status": "PASS" if not errors else "FAIL",
            "detail": f"{len(files)} file(s): {len(errors)} error-level, {len(warnings)} warning-level",
            "errors": errors[:20], "warnings": warnings[:20]}


def step_tests(build_dir, label, regex=None):
    if not os.path.isdir(build_dir):
        return {"step": f"UNIT TESTS ({label})", "status": "SKIP", "detail": "build dir absent"}
    cmd = [CTEST, "--test-dir", build_dir, "--output-on-failure"]
    if regex:
        cmd += ["-R", regex]
    rc, out = sh(cmd, timeout=3600)
    passed = out.count("Passed")
    failed = out.count("***Failed") + out.count("***Exception")
    summary = [l.strip() for l in out.splitlines() if "tests passed" in l or "tests failed" in l]
    return {"step": f"UNIT TESTS ({label})", "status": "PASS" if rc == 0 else "FAIL",
            "detail": f"{passed} passed, {failed} failed" + (f" | {summary[-1]}" if summary else "")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase", default="?")
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--files", nargs="*", default=None)
    ap.add_argument("--android", action="store_true", help="also gate the Android build")
    args = ap.parse_args()

    files = changed_files(args.files)
    results = []

    print("=" * 80)
    print(f"PHASE {args.phase} GATE — Stellarium Android/Cardboard port")
    print("=" * 80)
    print(f"changed source files vs upstream/master: {len(files)}")
    for f in files[:20]:
        print(f"    {f}")
    if len(files) > 20:
        print(f"    ...(+{len(files)-20})")
    print()

    if not args.skip_build:
        results.append(step_build(HOST_BUILD, "host"))
        if args.android:
            results.append(step_build(AND_BUILD, "android-arm64"))

    results.append(step_format(files))
    results.append(step_tidy(files))
    results.append(step_tests(HOST_BUILD, "host"))

    print("-" * 80)
    for r in results:
        print(f"  [{r['status']:4}] {r['step']:36} {r['detail']}")
        for line in r.get("per_file", [])[:8]:
            print(f"           {line}")
        for o in r.get("offenders", [])[:8]:
            print(f"           ! {o}")
        for e in r.get("errors", [])[:5]:
            print(f"           {e}")
    print("-" * 80)

    failed = [r for r in results if r["status"] == "FAIL"]
    skipped = [r for r in results if r["status"] == "SKIP"]
    npass = len(results) - len(failed) - len(skipped)
    print(f"\n  RESULT: {'GATE FAILED' if failed else 'GATE PASSED'}"
          f"   ({npass} pass, {len(failed)} fail, {len(skipped)} skip)")
    for r in failed:
        print(f"    - {r['step']}: {r['detail']}")

    out_dir = os.path.join(ROOT, "notes")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, f"phase{args.phase}-gate.json")
    json.dump({"phase": args.phase, "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
               "changed_files": files, "results": results},
              open(out_path, "w"), indent=1)
    print(f"\n  report -> {out_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
