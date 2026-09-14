import os, re, json, collections

ROOT = r"D:/Dev/STELLARIUM CARDBOARD/stellarium"
SKIP = {".git", "build", "_deps", "node_modules"}

def shaders():
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP]
        for fn in filenames:
            if fn.endswith((".vert", ".frag", ".geom", ".tesc", ".tese", ".comp")):
                yield os.path.join(dirpath, fn)

rows = []
for path in shaders():
    rel = os.path.relpath(path, ROOT).replace("\\", "/")
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    m = re.search(r"^\s*#version\s+([0-9]+)\s*(\w+)?", text, re.M)
    ver = f"{m.group(1)} {m.group(2) or ''}".strip() if m else "NONE"
    exts = re.findall(r"^\s*#extension\s+(\S+)", text, re.M)
    rows.append({
        "rel": rel,
        "dir": rel.rsplit("/", 1)[0],
        "ext": os.path.splitext(rel)[1],
        "ver": ver,
        "exts": exts,
        "lines": text.count("\n"),
        "uses_420pack": "GL_ARB_shading_language_420pack" in text,
        "uses_texture2D": bool(re.search(r"\btexture2D\s*\(", text)),
        "uses_textureProj": "textureProj" in text,
        "uses_shadow": "sampler2DShadow" in text or "samplerCubeShadow" in text,
        "uses_geom_builtin": bool(re.search(r"gl_PrimitiveID|EndPrimitive|EmitVertex", text)),
        "uses_layout": bool(re.search(r"\blayout\s*\(", text)),
        "uses_deriv": bool(re.search(r"dFdx|dFdy|fwidth", text)),
        "uses_discard": "discard" in text,
        "uses_uint": bool(re.search(r"\buint\b|\buvec[234]\b", text)),
        "uses_switch": "switch" in text,
    })

print(f"TOTAL SHADERS: {len(rows)}\n")

print("=== BY DIRECTORY (top) ===")
for d, c in collections.Counter(r["dir"] for r in rows).most_common(12):
    print(f"  {c:4}  {d}")

print("\n=== BY #version ===")
for v, c in collections.Counter(r["ver"] for r in rows).most_common():
    print(f"  {c:4}  #version {v}")

print("\n=== BY EXTENSION ===")
for e, c in collections.Counter(r["ext"] for r in rows).most_common():
    print(f"  {c:4}  {e}")

print("\n=== VERSION x TOP-LEVEL AREA ===")
def area(r):
    p = r["rel"]
    if p.startswith("atmosphere/"): return "atmosphere/ (ShowMySky)"
    if p.startswith("data/shaders/"): return "data/shaders/ (core)"
    if p.startswith("plugins/"): return "plugins/"
    return p.split("/")[0]
for (a, v), c in sorted(collections.Counter((area(r), r["ver"]) for r in rows).items()):
    print(f"  {c:4}  {a:32} #version {v}")

print("\n=== GLSL FEATURE USAGE (all shaders) ===")
feats = ["uses_420pack","uses_texture2D","uses_textureProj","uses_shadow",
         "uses_geom_builtin","uses_layout","uses_deriv","uses_discard","uses_uint","uses_switch"]
for f in feats:
    print(f"  {sum(1 for r in rows if r[f]):4}  {f}")

print("\n=== CORE SHADERS (data/shaders/) DETAIL ===")
for r in sorted(rows, key=lambda x: x["rel"]):
    if r["dir"] == "data/shaders":
        print(f"  {r['ver']:10} {r['ext']:6} {r['lines']:5}L  {os.path.basename(r['rel'])}")

json.dump(rows, open(r"D:/Dev/STELLARIUM CARDBOARD/notes/shader-audit.json", "w"), indent=1)
print("\nsaved -> D:/Dev/STELLARIUM CARDBOARD/notes/shader-audit.json")
