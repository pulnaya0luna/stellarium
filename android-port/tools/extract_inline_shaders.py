import os, re, json

ROOT = r"D:/Dev/STELLARIUM CARDBOARD/stellarium"
WORK = r"D:/Dev/STELLARIUM CARDBOARD/notes/gles_validation/inline"
os.makedirs(WORK, exist_ok=True)

COMMON = """#version 300 es
#define texture2D(a,b) texture(a,b)
#define texture2D_3(a,b,c) texture(a,b,c)
#define texture2DProj(a,b) textureProj(a,b)
#define texture2DProj_3(a,b,c) textureProj(a,b,c)
#define shadow2D_x(a,b) texture(a,b)
#define shadow2DProj_x(a,b) textureProj(a,b)
#define textureCube(a,b) texture(a,b)
#define textureCube_3(a,b,c) texture(a,b,c)
precision mediump float;
"""
VERT_TAIL = "#define ATTRIBUTE in\n#define VARYING out\n#line 1\n"
FRAG_TAIL = "#define VARYING in\n#define textureGrad_SUPPORTED\nout highp vec4 FRAG_COLOR;\n#line 1\n"

def strip_cpp_concat(body):
    """Turn C++ string-literal concatenation into plain GLSL text."""
    # join adjacent string literals:  "foo\n" "bar\n"  ->  foo\nbar\n
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    if lits:
        joined = "".join(lits)
        # unescape common C++ escapes
        joined = (joined.replace("\\n", "\n").replace("\\t", "\t")
                        .replace('\\"', '"').replace("\\\\", "\\"))
        return joined
    return body

def classify(src):
    if re.search(r"gl_Position", src) and not re.search(r"FRAG_COLOR\s*=", src):
        return "vert"
    if re.search(r"FRAG_COLOR", src) or re.search(r"gl_FragColor", src):
        return "frag"
    if re.search(r"gl_Position", src):
        return "vert"
    return None

extracted = []
for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, "src")):
    dirnames[:] = [d for d in dirnames if d not in {".git", "build"}]
    for fn in filenames:
        if not fn.endswith(".cpp"):
            continue
        path = os.path.join(dirpath, fn)
        text = open(path, encoding="utf-8", errors="replace").read()
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        for i, m in enumerate(re.finditer(r'R"\((.*?)\)"', text, re.S)):
            body = m.group(1)
            if not re.search(r"\bvoid\s+main\s*\(", body):
                continue
            stage = classify(body)
            if stage is None:
                continue
            # was a prefix call immediately before? (within 200 chars)
            before = text[max(0, m.start()-300):m.start()]
            if "globalShaderPrefix" in before:
                pm = re.findall(r"globalShaderPrefix\(StelOpenGL::(\w+)_SHADER\)", before)
                if pm:
                    stage = "vert" if pm[-1] == "VERTEX" else "frag"
            src = COMMON + (VERT_TAIL if stage == "vert" else FRAG_TAIL) + body
            name = f"{rel.replace('/', '_')}_{i}.{stage}"
            out = os.path.join(WORK, name)
            open(out, "w", encoding="utf-8").write(src)
            extracted.append({"src": rel, "idx": i, "stage": stage,
                              "file": out, "lines": body.count("\n")})

print(f"=== EXTRACTED INLINE GLSL BLOCKS: {len(extracted)} ===")
by_file = {}
for e in extracted:
    by_file.setdefault(e["src"], []).append(e)
for f, items in sorted(by_file.items(), key=lambda kv: -len(kv[1])):
    stages = ",".join(sorted({i["stage"] for i in items}))
    print(f"  {len(items):3}  {f:52} [{stages}]")

json.dump(extracted, open(os.path.join(WORK, "index.json"), "w"), indent=1)
print(f"\nsaved -> {WORK}")
