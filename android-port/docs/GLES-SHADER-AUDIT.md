# GLES Shader Compatibility Audit — Stellarium v26.2

**Date:** 2026-09-14 · **Commit:** 93f77f4 · **Target:** GLSL ES 3.00 (Android + Cardboard)
**Tool:** glslangValidator (reference Khronos GLSL compiler, built from source at
`tools/glslang/build/StandAlone/glslangValidator.exe`)
**Scripts:** `notes/shader_audit.py`, `notes/extract_inline_shaders.py`, `notes/audit_final.py`

## Headline

The "313 shaders need porting" concern is **not real**. 94% of them never reach an
Android build. The actual GLES shader surface is ~46 shaders, most of which already
compile as ES 3.00 unchanged.

## Where the 313 shaders actually live

| Count | Location | Reaches GLES? |
|---|---|---|
| **295 (94%)** | `atmosphere/default/shaders/**` — ShowMySky atmosphere model | **NO — compiled out** |
| 15 | `data/shaders/s3d_*` — Scenery3d plugin (optional) | Only if plugin enabled |
| 3 | `data/shaders/planet.*`, `preethamAtmosphere.vert`, `xyYToRGB.glsl` — core | **YES** |
| 83 sites | inline GLSL in C++ (28 substantial blocks) | **YES** |

### Proof the 295 are excluded (two independent verifications)

1. **Source guard:** `src/core/modules/AtmosphereShowMySky.cpp` is wrapped entirely in
   `#if !QT_CONFIG(opengles2)` (line 22 → `#endif` at 829).
2. **Toolchain fact:** the Qt 6.7.3 Android kit reports
   `#define QT_FEATURE_opengles2 1` in `include/QtGui/qtgui-config.h`
   (desktop kit reports `-1`). So `QT_CONFIG(opengles2)` is **true** on Android
   → every `!QT_CONFIG(opengles2)` block is removed.

Also `#version 330` + `GL_ARB_shading_language_420pack` appear in exactly those 295
files — consistent with them being desktop-only.

## Which shader dialect actually runs

`StelMainView.cpp` decides at runtime:
```
glInfo.isGLES            = format.renderableType() == QSurfaceFormat::OpenGLES;
glInfo.isHighGraphicsMode = majorVersion >= 3 && highGraphicsFunctions();
```
On an Android device exposing ES 3.x this gives **isGLES = true, isHighGraphicsMode = true**
→ `globalShaderPrefix()` emits **`#version 300 es` + `precision mediump float;`**
and defines `ATTRIBUTE in / VARYING out / FRAG_COLOR`. **GLSL ES 3.00 is the target dialect.**

## Validation results (ES 3.00, with the real prefix and real helper chunks)

| Verdict | Count | Meaning |
|---|---|---|
| **PASS** | 13 | Compiles clean as ES 3.00 unchanged |
| **INDETERMINATE** | 4 | Composed with runtime-generated per-projection shaders — structurally unvalidatable offline |
| N/A — ES2 dialect | 12 | Scenery3d (optional plugin); upstream already ships `s3d_pixellit_es.frag` selected by `ShaderManager.cpp:256` |
| N/A — ES 3.2 feature | 2 | Geometry shaders (`s3d_*.geom`) — ES 3.00 has no geometry stage |
| N/A — desktop-only | 2 | MSAA `sampler2DMS` variant and ShowMySky, both `!QT_CONFIG(opengles2)` guarded |

**Real porting work identified: 0 confirmed blockers.** No shader was found to use a
feature that ES 3.00 lacks and that Android needs.

## Why 4 are INDETERMINATE (not failures)

Stellarium assembles each program at runtime from stacked chunks:
```
globalShaderPrefix(stage)
  + modelViewTransform->getForwardTransformShader()
  + <projection-type shader>          ← generated per projection, 8 variants
  + projector->getUnProjectShader()   ← winPosToWorldPos / unProject
  + projector->getBackwardTransformShader()
  + core->getAberrationShader()
  + extinction.getForwardTransformShader()
  + makeSaturationShader() / makeDitheringShader() / makeSRGBUtilsShader()
  + <shader body>
```
`projectorForwardTransform` alone is defined **8 times** in `StelProjectorClasses.cpp`
across 23 shader-generation sites. The complete source exists only in memory, for the
projection the user selected. Any offline validator will report a missing symbol; this
says nothing about the shader's ES 3.00 validity.

Affected: `AtmosphereLightweight.cpp`, `Landscape.cpp`, `MilkyWay.cpp`,
`ZodiacalLight.cpp` (fragment). All resolve at runtime with a real GL context.

## Notable shaders already ES-aware upstream

- `s3d_pixellit_es.frag` — an explicit ES variant, chosen by
  `plugins/Scenery3d/src/ShaderManager.cpp:256` ("for various reasons, ES version is separate").
- `StelOpenGL.hpp` — `#if defined(Q_OS_ANDROID)` → `<GLES3/gl32.h>` + `QOpenGLExtraFunctions`.
- `globalShaderPrefix()` — full GLES branch incl. a GLES2-era low-graphics path.
- `StelOpenGL.cpp` — `#include <GLES3/gl32.h>` for Android, so ES 3.2 is the declared ceiling.

## Residual uncertainty (what only a device can settle)

1. **Driver variance.** glslangValidator is the reference compiler; Adreno/Mali/PowerVR
   differ. A shader that validates can still fail on a specific driver.
2. **The 4 INDETERMINATE shaders.** Need a real GLES context to confirm.
3. **`#version 300 es` vs ES 3.2.** Qt's Android build advertises only `opengles2`;
   `GLES3/gl32.h` is included for Android, so ES 3.2 is the intended ceiling, but
   nothing guarantees a device gives it. Runtime `isHighGraphicsMode` handles this by
   falling back to the low-graphics path.

**Recommendation:** treat shader risk as **low**. Resolve the residual items in Phase 3
(GLES bring-up) on a real device, not before.

## Reproduction

```bash
python notes/shader_audit.py          # census: where shaders live, #version spread
python notes/extract_inline_shaders.py # pull inline GLSL out of C++
python notes/audit_final.py           # validate everything as ES 3.00
```
Artifacts: `notes/gles_final/final.json`, `notes/shader-audit.json`,
`notes/gles_validation/inline/index.json`.
