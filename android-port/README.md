# android-port — Stellarium Android / Cardboard VR port

Everything in this directory is **fork-specific**. It does not exist upstream, so it
never conflicts with `git rebase upstream/master` — which is the whole point of keeping
it in one top-level folder.

Upstream's own `android/` directory is **untouched** (it holds the packaging manifest and
resources used by `QT_ANDROID_PACKAGE_SOURCE_DIR`). Do not confuse the two.

## Layout

| Path | What it is |
|---|---|
| `phase_gate.py` | The mandated post-phase gate: BUILD → LINT → STATIC ANALYSIS → UNIT TESTS → REPORT |
| `clang-tidy-port.yaml` | Port clang-tidy config (see "Why a separate config" below) |
| `docs/GLES-SHADER-AUDIT.md` | GLSL ES 3.00 compatibility audit of the whole shader surface |
| `tools/shader_audit.py` | Census: where shaders live, `#version` spread |
| `tools/extract_inline_shaders.py` | Pulls inline GLSL out of C++ raw-string blocks |
| `tools/audit_final.py` | Validates every GLES-relevant shader as ES 3.00 |

## Running the gate

```bash
python android-port/phase_gate.py --phase 2            # build + lint + tidy + tests
python android-port/phase_gate.py --phase 2 --skip-build
python android-port/phase_gate.py --phase 2 --android  # also gate the Android build
```

Thresholds enforced: 0 build errors, 0 error-level lint, 0 newly-misformatted files,
0 failed tests. The report lands in `../notes/phase<N>-gate.json`.

### Baseline-diff linting (why it works this way)

Upstream Stellarium is **not** clang-format clean under a current clang-format, and its
`.clang-tidy` cannot be parsed by modern clang-tidy at all. Linting the tree naively
therefore reports hundreds of pre-existing issues and tells you nothing about your own
work. The gate instead:

- **clang-format**: compares each file's violation count against its pristine
  `upstream/master` version. A file fails only if it is *newly* misformatted. Files this
  port **adds** must be fully clean.
- **clang-tidy**: runs `clang-tidy-port.yaml` on **changed files only**.

## Why a separate clang-tidy config

Upstream's `.clang-tidy` sets `AnalyzeTemporaryDtors`, a key **removed in clang-tidy 12+**:

```
.clang-tidy:5:1: error: unknown key 'AnalyzeTemporaryDtors'
```

With LLVM 22 that is a hard parse failure, so upstream's config is unusable. Upstream also
enables `'*'` (every check), which is pure noise in practice. `clang-tidy-port.yaml`
enables defect-focused families instead: `clang-analyzer-*`, `bugprone-*`, `performance-*`,
plus selected `modernize-`/`misc-`/`readability-` checks — weighted toward what matters for
a VR renderer (memory, undefined behaviour, per-frame cost) rather than formatting opinions.

## Build prerequisites (verified working)

- **clang-tidy cannot read MSVC precompiled headers.** The host tree must be configured
  with `-DENABLE_PCH=0` or the static-analysis step dies with
  `file '...cmake_pcm.cxx.pch' is not a valid PCH file`.
- Host build needs `vcvars64.bat` sourced first, `ENABLE_EXPORT_COMPILE_COMMANDS=1`
  (for clang-tidy), and Qt's `bin` on `PATH` to *run* the test binaries
  (otherwise they exit `0xc0000135`, STATUS_DLL_NOT_FOUND).
- Android build needs `QT_HOST_PATH` (desktop kit) + `ANDROID_NDK_ROOT` (r27c) + the Qt
  Android toolchain file.

Exact commands: see the `stellarium-android-port` skill.
