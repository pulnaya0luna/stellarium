"""DEFINITIVE GLES 3.00 audit — every helper chunk reproduced from real source."""
import os, re, subprocess, json

ROOT = r"D:/Dev/STELLARIUM CARDBOARD/stellarium"
VAL  = r"D:/Dev/STELLARIUM CARDBOARD/tools/glslang/build/StandAlone/glslangValidator.exe"
WORK = r"D:/Dev/STELLARIUM CARDBOARD/notes/gles_final"
os.makedirs(WORK, exist_ok=True)

COMMON = ("#version 300 es\n"
 "#define texture2D(a,b) texture(a,b)\n#define texture2D_3(a,b,c) texture(a,b,c)\n"
 "#define texture2DProj(a,b) textureProj(a,b)\n#define texture2DProj_3(a,b,c) textureProj(a,b,c)\n"
 "#define shadow2D_x(a,b) texture(a,b)\n#define shadow2DProj_x(a,b) textureProj(a,b)\n"
 "#define textureCube(a,b) texture(a,b)\n#define textureCube_3(a,b,c) texture(a,b,c)\n"
 "precision mediump float;\n")
VT = "#define ATTRIBUTE in\n#define VARYING out\n#line 1\n"
FT = "#define VARYING in\n#define textureGrad_SUPPORTED\nout highp vec4 FRAG_COLOR;\n#line 1\n"

def src_chunk(relpath, marker="R\"(", start=0):
    """Pull a raw-string GLSL chunk out of a C++ file, verbatim."""
    t = open(os.path.join(ROOT, relpath), encoding="utf-8", errors="replace").read()
    i = t.find(marker, start)
    if i < 0: return ""
    j = t.find(")\"", i)
    return t[i+len(marker):j]

UNPROJ     = src_chunk("src/core/StelProjector.cpp", "1+R\"(")
ABERRATION = src_chunk("src/core/StelCore.cpp", "1+R\"(")
EXTINCTION = src_chunk("src/core/RefractionExtinction.cpp", "1+R\"(")
SRGB       = src_chunk("src/core/StelSRGB.cpp", "1+R\"(")
DITHER     = src_chunk("src/core/Dithering.cpp", "1+R\"(")
XYY        = open(os.path.join(ROOT, "data/shaders/xyYToRGB.glsl"), encoding="utf-8", errors="replace").read().split("*/",1)[-1]

# the C++ substitutes these enum names with integers
EXTINCTION = (EXTINCTION.replace("UndergroundExtinctionZero", "0")
                         .replace("UndergroundExtinctionMax", "1")
                         .replace("UndergroundExtinctionMirror", "2"))

def helpers(body):
    h = ""
    if re.search(r"unProject\(|winPosToWorldPos\(|worldPosToAltAzPos\(", body):
        h += UNPROJ + "\n"
    if "applyAberrationToViewDir(" in body or "applyAberrationToObject(" in body:
        h += ABERRATION + "\n"
    if "extinctionMagnitude(" in body or "EXTINCTION_airmass(" in body:
        h += EXTINCTION + "\n"
    if re.search(r"\bdither\s*\(", body):
        h += DITHER + "\n"
    if "srgbToLinear" in body or "linearToSRGB" in body:
        h += SRGB + "\n"
    return h or None

def run(name, stage, src):
    p = os.path.join(WORK, name)
    open(p, "w", encoding="utf-8").write(src)
    r = subprocess.run([VAL, "-S", stage, p], capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    if "ERROR" in out:
        for ln in out.splitlines():
            if "ERROR:" in ln:
                return False, ln.split("ERROR:", 1)[1].strip()[:120]
        return False, "error"
    return True, ""

def assemble(stage, body, extra=None):
    return COMMON + (extra or "") + (VT if stage == "vert" else FT) + body

rows = []

# ---- file shaders ----
for fn in sorted(os.listdir(os.path.join(ROOT, "data/shaders"))):
    if not fn.endswith((".vert", ".frag", ".geom")): continue
    stage = os.path.splitext(fn)[1].lstrip(".")
    body = open(os.path.join(ROOT, "data/shaders", fn), encoding="utf-8", errors="replace").read()
    if stage == "geom":
        rows.append((fn, "N/A-ES3.2", "geometry stage absent in ES 3.00")); continue
    if fn.startswith("s3d"):
        rows.append((fn, "N/A-ES2" if "_es" not in fn else "N/A-desktop",
                     "Scenery3d ES2 dialect (optional plugin)" if "_es" not in fn else "this IS the ES variant")); continue
    extra = helpers(body)
    if "xyYToRGB" in body or fn == "preethamAtmosphere.vert":
        extra = (extra or "") + XYY
    ok, err = run("f_" + fn, stage, assemble(stage, body, extra))
    rows.append((fn, "PASS" if ok else "FAIL", err))

# ---- inline shaders ----
idx = json.load(open(r"D:/Dev/STELLARIUM CARDBOARD/notes/gles_validation/inline/index.json"))
done = {}
for e in idx:
    body = open(e["file"], encoding="utf-8").read().split("#line 1\n", 1)[-1]
    key = (e["src"].split("/")[-1], e["stage"])
    if key in done: continue
    if "AtmosphereShowMySky" in e["src"]:
        done[key] = ("N/A-desktop", "ShowMySky compiled out on GLES"); continue
    if "sampler2DMS" in body:
        done[key] = ("N/A-desktop", "MSAA variant, !QT_CONFIG(opengles2) guarded"); continue
    if "layout(lines)" in body or "layout(triangles)" in body:
        done[key] = ("N/A-ES3.2", "geometry shader (wide-line emulation)"); continue
    ok, err = run("i_" + os.path.basename(e["file"]), e["stage"], assemble(e["stage"], body, helpers(body)))
    done[key] = ("PASS", "") if ok else ("FAIL", err)

for (f, st), (v, d) in done.items():
    rows.append((f"{f} [{st}]", v, d))

order = {"PASS":0,"FAIL":1,"INDETERMINATE":2,"N/A-desktop":3,"N/A-ES2":4,"N/A-ES3.2":5}
print("="*84); print("STELLARIUM → GLSL ES 3.00 — DEFINITIVE AUDIT"); print("="*84)
for n, v, d in sorted(rows, key=lambda r:(order.get(r[1],9), r[0])):
    print(f"  {v:13} {n:46} {d[:62]}")
c = {}
for _, v, _ in rows: c[v] = c.get(v,0)+1
print("\n  TOTALS:", ", ".join(f"{k}={v}" for k,v in sorted(c.items())))
fails = [r for r in rows if r[1]=="FAIL"]
print(f"\n  REAL PORTING WORK: {len(fails)} shader(s)")
for n,v,d in fails: print(f"    - {n}: {d}")
json.dump(rows, open(os.path.join(WORK,"final.json"),"w"), indent=1)
print(f"\n  report -> {WORK}/final.json")
