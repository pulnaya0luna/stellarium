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

## Android build: two known constraints

**1. The APK step cannot use upstream's `--release` path yet.** Upstream's CMake invokes
`androiddeployqt ... --release`, which (a) requires a signing keystore this port does not
have, and (b) makes Gradle pick the newest installed SDK platform — currently
`platforms;android-37`, which the Qt-bundled AGP rejects and which the SDK names
`android-37.0` so Gradle cannot match it either. Compilation itself is unaffected.

Build the APK on the debug path instead (auto-signed with the debug key):

```bash
cd build/android-arm64/src
export JAVA_HOME="C:/Program Files/Eclipse Adoptium/jdk-17.0.20.101-hotspot"
androiddeployqt --input android-stellarium-deployment-settings.json \
  --output android-build --apk android-build/stellarium.apk \
  --android-platform android-34 --debug
```

Verified output: `android-build/build/outputs/apk/debug/android-build-debug.apk` —
44 MB, 142 files, 38 `arm64-v8a` `.so`s including `libstellarium_arm64-v8a.so` (36 MB).

**2. Qt 6.7.3's bundled Android Gradle Plugin is too old.** Its template pins
`com.android.tools.build:gradle:7.4.1` (caps at compileSdk 33) while Qt's own bundled
AndroidX libs need ≥ 34 → hard failure. Patched to **AGP 8.2.2** in
`D:/Qt/6.7.3/android_<arch>/src/android/templates/build.gradle` (original kept beside it as
`build.gradle.orig-upstream-7.4.1`). **This is a Qt-install patch, not a repo change** —
re-apply it after any Qt reinstall, and mirror it in `android_x86_64` if that kit is used.

**Device/emulator note:** the APK is **arm64-v8a only**, while the installed AVD
(`Medium_Phone_API_36.1`) is **x86_64** — it cannot run this APK. Either build the
`android_x86_64` kit for emulator testing, or use a physical arm64 device.
The x86_64 kit build + install is verified working; use
`android-port/tools/push-device-data.sh` to deploy the runtime data.

## Running on Android: the app needs its data on external storage

First launch dies in `StelFileMgr::init()` with
`FATAL Couldn't find install directory location.` Stellarium expects a desktop
install layout that does not exist on Android. Upstream *does* already have an
Android branch in that function, searching `assets:`, `/storage/*/stellarium`,
`/storage/*/0/stellarium` and `/sdcard/stellarium` — so the data has to be placed
there. Use `android-port/tools/push-device-data.sh`, which handles permissions and
all three data sets and verifies them.

**Three data sets are required, and missing ones do not fail cleanly:**

| Missing | Symptom |
|---|---|
| `data/` | `FATAL Couldn't find install directory location.` — it validates `data/ssystem_major.ini` |
| `landscapes/` | **Stack overflow** (512 frames, all `LandscapeMgr::setCurrentLandscapeID+722`): with `zero` absent, the "unknown landscape → use 'zero'" fallback at `LandscapeMgr.cpp:920-923` recurses into itself forever |
| `textures/` | **SIGSEGV** in `Planet::drawSphere` → `StelTexture::bind`: `rings->tex->bind(2)` (`Planet.cpp:5271,5494`) is unguarded, so Saturn's ring texture failing to load crashes the first frame |

Also required: **`MANAGE_EXTERNAL_STORAGE` granted with `--uid`**. A plain
`appops set` writes the non-uid mode and does *not* take effect; without it the app
is denied read access to `/sdcard/stellarium` and dies in `copyDefaultConfigFile()`
with `ERROR copyDefaultConfigFile failed to copy file .../default_cfg.ini`.

**Verified running:** Stellarium launches on the x86_64 emulator, loads 895,910
minor-planet and 2,069 comet records, creates its scene FBO at 1079x2273, and
renders the sky (Moon and Venus labelled) over the Guereins landscape.

Known cosmetic issue: network downloads fail with `TLS initialization failed`
(Stellarium's bundled CA/OpenSSL setup), so satellite, nova and exoplanet updates
do not run. Does not affect rendering.

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
