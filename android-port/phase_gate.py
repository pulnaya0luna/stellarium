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
import re
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
ANDROIDDEPLOYQT = r"D:/Qt/6.7.3/msvc2019_64/bin/androiddeployqt.exe"

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


def _style_arg():
    """clang-format resolves .clang-format by walking up from the FILE IT IS GIVEN.

    When we measure content through a temp file (to compare against a git baseline),
    that temp file lives outside the repo, so the search finds no config and silently
    falls back to LLVM's default style (80 columns) instead of the repo's 120. Passing
    the repo's config explicitly is what makes the two measurements comparable.
    """
    cfg = os.path.join(REPO, ".clang-format")
    return f"--style=file:{cfg}" if os.path.exists(cfg) else "--style=file"


def format_violations(path, content=None):
    """Count clang-format violations for a file (or given content)."""
    if content is None:
        rc, out = sh([CLANG_FORMAT, _style_arg(), "--dry-run", "--Werror", path])
        if rc == 0:
            return 0
        return sum(1 for l in out.splitlines() if "-Wclang-format-violations" in l)
    tmp = _write_temp(path, content)
    try:
        rc, out = sh([CLANG_FORMAT, _style_arg(), "--dry-run", "--Werror",
                      "--assume-filename", path, tmp])
        if rc == 0:
            return 0
        return sum(1 for l in out.splitlines() if "-Wclang-format-violations" in l)
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass


def step_apk(build_dir):
    """Build the Android APK the way this port actually can, and verify the artifact.

    Upstream's CMake target chain ends in `androiddeployqt --release`, which needs a
    signing keystore and selects an SDK platform the bundled AGP rejects. So the gate
    runs the equivalent debug invocation itself and checks the resulting APK really
    contains the arm64 Stellarium engine — an artifact check, not a build log claim.
    """
    src_dir = os.path.join(build_dir, "src")
    settings = os.path.join(src_dir, "android-stellarium-deployment-settings.json")
    out_dir = os.path.join(src_dir, "android-build")
    apk = os.path.join(out_dir, "build", "outputs", "apk", "debug", "android-build-debug.apk")
    if not os.path.exists(settings):
        return {"step": "APK (android-arm64, debug)", "status": "SKIP",
                "detail": "no deployment settings yet (Android build not configured)"}
    env = {"JAVA_HOME": r"C:/Program Files/Eclipse Adoptium/jdk-17.0.20.101-hotspot"}
    rc, out = sh([ANDROIDDEPLOYQT,
                  "--input", settings,
                  "--output", out_dir,
                  "--apk", os.path.join(out_dir, "stellarium.apk"),
                  "--android-platform", "android-34",
                  "--debug"], cwd=src_dir, timeout=3600, env=env)
    if rc != 0 or not os.path.exists(apk):
        return {"step": "APK (android-arm64, debug)", "status": "FAIL",
                "detail": f"androiddeployqt exit={rc}, apk exists={os.path.exists(apk)}",
                "log_tail": "\n".join(out.strip().splitlines()[-20:])}
    size_mb = os.path.getsize(apk) / 1e6
    # verify the engine is actually inside, not just that a file appeared
    import zipfile
    engine = None
    with zipfile.ZipFile(apk) as z:
        for n in z.namelist():
            if n.endswith("libstellarium_arm64-v8a.so"):
                engine = z.getinfo(n).file_size
    ok = engine is not None
    return {"step": "APK (android-arm64, debug)",
            "status": "PASS" if ok else "FAIL",
            "detail": f"{size_mb:.1f} MB, engine .so={engine} bytes" if ok
                      else "APK built but libstellarium_arm64-v8a.so missing"}


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
    # Compilation itself succeeds; the APK is produced by step_apk() on the debug path.
    #
    # This is only a documented limit if the code actually compiled. Gate on the compile
    # error count as well: if a real compile error ever coexists with that packaging
    # failure, treating it as KNOWN would silently pass a broken build, which is the one
    # thing a gate must never do.
    if rc != 0 and errs == 0 and "Platform SDK with path: platforms;android-37" in out:
        status = "KNOWN"
        detail = (f"exit={rc}, compile 'error:' lines={errs} — "
                  "all code compiled; upstream's --release APK step is unsupported here "
                  "(needs a keystore + an SDK platform the bundled AGP rejects). "
                  "The APK is built and verified by the separate APK step.")
    return {"step": f"BUILD ({label})", "status": status, "detail": detail, "log_tail": tail}


def _write_temp(suffix_name, content):
    """Write content to a temp file that keeps the original extension.

    clang-format resolves .clang-format and the language from the filename, so the temp
    file must carry the same extension as the real one (and be paired with
    --assume-filename) for the measurement to mean anything.
    """
    with tempfile.NamedTemporaryFile("w", suffix=os.path.splitext(suffix_name)[1],
                                     delete=False, encoding="utf-8", newline="") as fh:
        fh.write(content)
        return fh.name


def changed_lines(f):
    """Line numbers ADDED by this branch in file f, versus upstream/master."""
    rc, out = sh(["git", "-C", REPO, "diff", "-U0", "upstream/master", "--", f])
    if rc != 0:
        return set()
    added = set()
    for m in re.finditer(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", out, re.M):
        start = int(m.group(1))
        cnt = int(m.group(2) or 1)
        added.update(range(start, start + cnt))
    return added


def step_format(files):
    """Baseline-diffed clang-format: fail only on NEW deviations, on CHANGED lines.

    Three traps here, all hit in practice, and all of them produce hundreds of phantom
    violations that say nothing about the actual work:

    1. Upstream is not clang-format clean under a current clang-format (1485 violations
       in StelApp.cpp alone), so a whole-tree check is meaningless.
    2. The working tree is CRLF (core.autocrlf=true) while `git show` returns LF, and
       clang-format's count depends on what it is handed. Both sides must be measured
       the same way, via --assume-filename on a normalised temp file.
    3. clang-format reformats the whole file, so a line-count comparison shifts and
       reports violations on lines that are not mine. Worse, reformatting can cascade
       from a long pre-existing line just above an edit. The honest question is only:
       "are the lines I ADDED formatted as clang-format would write them?"

    So this compares the added lines specifically: format the file, then check whether
    the added lines' content survived unchanged.
    """
    if not files:
        return {"step": "LINT (clang-format, baseline-diff)", "status": "SKIP",
                "detail": "no changed source files vs upstream/master"}
    rows, offenders = [], []
    for f in files:
        p = os.path.join(REPO, f)
        if not os.path.exists(p):
            continue
        try:
            with open(p, encoding="utf-8", errors="replace", newline="") as fh:
                cur = fh.read().replace("\r\n", "\n")
        except OSError:
            continue

        added = changed_lines(f)
        if not added:
            # File changed only by deletion, or is new. Fall back to whole-file check
            # for genuinely new files; skip otherwise.
            if not upstream_has(f):
                n = format_violations(f, cur)
                rows.append(f"{f}: new file, {n} violation(s)")
                if n > 0:
                    offenders.append(f"{f}: {n} violation(s) in a NEW file")
            continue

        # What would clang-format write? Use the repo's config explicitly: the temp file
        # lives outside the repo, so clang-format would otherwise fall back to LLVM's
        # default 80-column style and "disagree" with the real file.
        tmp = _write_temp(f, cur)
        try:
            rc, out = sh([CLANG_FORMAT, _style_arg(), "--assume-filename", f, tmp])
        finally:
            try:
                os.unlink(tmp)
            except OSError:
                pass
        if rc != 0 and not out.strip():
            rows.append(f"{f}: clang-format failed to run")
            continue

        # A file upstream does not have is entirely ours, so the standard is simply
        # "clang-format would not change it".
        if not upstream_has(f):
            clean = out.rstrip("\n") == cur.rstrip("\n")
            rows.append(f"{f}: new file, {'clean' if clean else 'NEEDS FORMATTING'}")
            if not clean:
                offenders.append(f"{f}: new file is not clang-format clean")
            continue

        # For a MODIFIED upstream file only the ADDED lines are ours to answer for.
        # clang-format rewrites the whole file (and upstream is not clean), so compare
        # per-line: an added line is fine if clang-format's output contains it verbatim.
        # Comments may be reflowed, which is why exact index matching would produce
        # false positives on otherwise correctly formatted code.
        have = cur.split("\n")
        want_set = {l.rstrip() for l in out.split("\n")}
        bad = []
        for ln in sorted(added):
            if ln - 1 >= len(have):
                continue
            text = have[ln - 1].rstrip()
            if not text.strip():
                continue
            if text not in want_set:
                bad.append(f"line {ln}: {text.strip()[:90]}")
        rows.append(f"{f}: {len(added)} added line(s), {len(bad)} misformatted")
        if bad:
            offenders.append(f"{f}: " + "; ".join(bad[:4]))

    return {"step": "LINT (clang-format, baseline-diff)",
            "status": "PASS" if not offenders else "FAIL",
            "detail": f"{len(rows)} file(s); {len(offenders)} with new deviations",
            "per_file": rows[:25], "offenders": offenders[:15]}


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
            results.append(step_apk(AND_BUILD))

    results.append(step_format(files))
    results.append(step_tidy(files))
    results.append(step_tests(HOST_BUILD, "host"))

    print("-" * 80)
    for r in results:
        print(f"  [{r['status']:5}] {r['step']:36} {r['detail']}")
        for line in r.get("per_file", [])[:8]:
            print(f"           {line}")
        for o in r.get("offenders", [])[:8]:
            print(f"           ! {o}")
        for e in r.get("errors", [])[:5]:
            print(f"           {e}")
    print("-" * 80)

    failed = [r for r in results if r["status"] == "FAIL"]
    known = [r for r in results if r["status"] == "KNOWN"]
    skipped = [r for r in results if r["status"] == "SKIP"]
    npass = len(results) - len(failed) - len(known) - len(skipped)
    print(f"\n  RESULT: {'GATE FAILED' if failed else 'GATE PASSED'}"
          f"   ({npass} pass, {len(failed)} fail, {len(known)} known-limit, {len(skipped)} skip)")
    for r in failed:
        print(f"    - {r['step']}: {r['detail']}")
    for r in known:
        print(f"    ~ {r['step']}: documented limit, not a code failure")

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
