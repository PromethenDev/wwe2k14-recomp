"""Direct3D 11 shaders for wwe13_d3d11.exe (from WWE '12 PC, tools/convert_shaders_d3d11.py): the game's shaders as DXBC (Shader Model 5.0, fxc).

DXIL can't become DXBC, so this starts again from the HLSL that reblue-XenosRecomp writes (its 4th
argument, a folder: one .hlsl per shader, tools/gen_shaders.ps1 does the rest) and compiles it with
the Windows SDK's fxc. Method and fixes adapted, with his permission, from the SvR 2011 PC port
(KaikoClanworth1, port/tools/convert_shaders.py): fxc's older HLSL, the finite clamps.

What changes for D3D11 (no descriptor heaps):
- each fetch slot has its own register, per stage: 2D t0-15, cube t16-31, 3D t32-47, samplers
  s0-15 (slot & 15: the pixel shaders use slots 0-15, the vertex shaders 16-17), so a shader's
  "<name>_Texture2DDescriptorIndex" becomes that literal number (slot & 15, the index in its
  kind's register array);
- the shared constants keep their layout, but where the descriptor indices were, the renderer
  writes each slot's swizzle (D3D11 views have none): 3 bits a component, 0-3 xyzw, 4 zero,
  5 one (0x688 = xyzw), applied after each fetch;
- the spec constants are compiled in: one DXBC per subset of the shader's mask, like the D3D12
  prelinked cache (port/src/prelink).

Output: a C++ file with the zlib-compressed DXBC blobs and an index sorted by (hash, constants).
Nothing of the game goes in the repository: CMake writes the output in the build folder (not versioned).

    python -I port/tools/convert_shaders_d3d11.py <hlsl folder> <out.cpp> [-j N] [--keep <folder>]
"""
import argparse
import concurrent.futures
import glob
import os
import re
import subprocess
import sys
import tempfile
import zlib

FXC = os.environ.get("WWE13_FXC") or (sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\fxc.exe")) or [""])[-1]  # WWE13_FXC: Linux cross-build (fxc-lite)

HEADER_RE = re.compile(r"^// (pixel|vertex) shader\s+hash=0x([0-9A-Fa-f]{16})\s+specConstants=0x([0-9A-Fa-f]+)")
INDEX_RE = re.compile(r"^\s*uint (\w+)_(Texture2D|Texture3D|TextureCube|Sampler)DescriptorIndex : "
                      r"packoffset\(c(\d+)\.([xyzw])\);\s*$", re.M)
# Constant registers of the four index tables, slots 0-15 then 16-31.
KIND_BASE = {"Texture2D": 0, "Texture3D": 4, "TextureCube": 8, "Sampler": 12}
ARRAY_MACRO_RE = re.compile(r"#define (\w+)\(INDEX\)")

SWIZZLE = r'''
// D3D11: the slot's swizzle (written by the renderer where the descriptor indices were).
float SvrComponent(float4 v, uint c)
{
    return c < 4 ? dot(v, float4(c == 0, c == 1, c == 2, c == 3)) : (c == 5 ? 1.0 : 0.0);
}

float4 SvrSwizzle(float4 v, uint code)
{
    [branch] if (code == 0x688)
        return v;
    return float4(SvrComponent(v, code & 7), SvrComponent(v, (code >> 3) & 7),
                  SvrComponent(v, (code >> 6) & 7), SvrComponent(v, (code >> 9) & 7));
}

// Slot R of a kind (its constants at c0 2D, c4 3D, c8 cube) -> its swizzle.
#define SVR_SWZ(BASE, R) SVR_SWZ_TABLE[(BASE) + ((R) >> 2)][(R) & 3]
#define tfetch2D(R, S, UV, O) SvrSwizzle(tfetch2DRaw(R, S, UV, O), SVR_SWZ(0, R))
#define tfetch2DBicubic(R, S, UV, O) SvrSwizzle(tfetch2DBicubicRaw(R, S, UV, O), SVR_SWZ(0, R))
#define tfetch3D(R, S, UV) SvrSwizzle(tfetch3DRaw(R, S, UV), SVR_SWZ(4, R))
#define tfetchCube(R, S, UV, CUBE) SvrSwizzle(tfetchCubeRaw(R, S, UV, CUBE), SVR_SWZ(8, R))
'''

# fxc keeps clamp(x, FLT_MIN, FLT_MAX) out as it takes floats as finite: rsqrt(0) stayed
# infinite and 0 * inf gave NaN (white ringside floor in Kaiko's D3D11). The clamp works on the
# float's bits with integer operations (nothing fxc or a driver can fold away): the magnitude is
# capped at FLT_MAX and the sign kept, so +-inf (and NaN) become +-FLT_MAX and finite values pass
# bit for bit. The first version, a `precise` float clamp, broke the wrestlers' skin on NVIDIA
# Fermi GPUs (GeForce GT 430, #55): Kaiko's fix for his SvR 2011 port (commit d387f29), adapted.
FINITE_HELPER = ("float SvrFinite(float x) { uint u = asuint(x); "
                 "return asfloat((u & 0x80000000u) | min(u & 0x7fffffffu, 0x7f7fffffu)); }\n")
FINITE_TAIL = ", FLT_MIN, FLT_MAX)"


def finite_calls(text):
    out, pos = [], 0
    while True:
        start = text.find("clamp(", pos)
        if start < 0:
            break
        depth, k = 1, start + len("clamp(")
        while k < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[k], 0)
            k += 1
        call = text[start:k]
        if call.endswith(FINITE_TAIL):
            inner = finite_calls(call[len("clamp("):-len(FINITE_TAIL)])
            out.append(text[pos:start] + "SvrFinite(" + inner + ")")
            pos = k
        else:
            out.append(text[pos:start + len("clamp(")])
            pos = start + len("clamp(")
    out.append(text[pos:])
    return "".join(out)


MEMBER_RE = re.compile(r"^\s*float4 (\w+)(\[\d+\])? : packoffset\(c(\d+)\);\s*$", re.M)


def whole_constant_file(text, cbuffer, array):
    """cbuffer <cbuffer> { float4 NAME : packoffset(cK); float4 NAME_a[N] : packoffset(cK); ... }
    -> cbuffer <cbuffer> { float4 <array>[256]; }, NAME -> <array>[K], NAME_a[i] -> <array>[K + i]."""
    m = re.search(r"cbuffer %s : register\(([^)]*)\)\s*\{(.*?)\};" % cbuffer, text, re.S)
    if not m:
        return text
    body = m.group(2)
    scalars, arrays = {}, {}
    for name, is_array, reg in MEMBER_RE.findall(body):
        (arrays if is_array else scalars)[name] = int(reg)
    # (the #define wrappers inside the block stay, before the block)
    defines = "".join(l + "\n" for l in body.splitlines() if l.strip().startswith("#define"))
    replacement = "cbuffer %s : register(%s)\n{\n\tfloat4 %s[256];\n};\n%s" % (cbuffer, m.group(1), array, defines)
    text = text[:m.start()] + replacement + text[m.end():]
    for name, reg in arrays.items():
        text = re.sub(r"\b%s\[" % re.escape(name), "%s[%d + " % (array, reg), text)
    for name, reg in scalars.items():
        text = re.sub(r"\b%s\b" % re.escape(name), "%s[%d]" % (array, reg), text)
    return text


def d3d11_source(text, vertex):
    """The dumped HLSL as fxc / D3D11 wants it."""
    # Heaps -> one register a slot.
    for kind, reg in (("Texture2D", "t0"), ("TextureCube", "t16"), ("Texture3D", "t32")):
        text = re.sub(r"%s<float4> g_%sDescriptorHeap\[\] : register\(t0, space\d\);" % (kind, kind),
                      "%s<float4> g_%sDescriptorHeap[16] : register(%s);" % (kind, kind, reg), text)
    text = re.sub(r"SamplerState g_SamplerDescriptorHeap\[\] : register\(s0, space\d\);",
                  "SamplerState g_SamplerDescriptorHeap[16] : register(s0);", text)
    # The shared constants: indices -> literal registers, their places -> the swizzle tables.
    head, sep, tail = text.partition("cbuffer SharedConstants")
    if not sep:
        raise ValueError("no SharedConstants")
    block, sep2, rest = tail.partition("};")
    defines = []
    for name, kind, c, comp in INDEX_RE.findall(block):
        c = int(c)
        slot = (c - KIND_BASE[kind]) * 4 + "xyzw".index(comp) if c < 24 else \
            16 + (c - 24 - KIND_BASE[kind]) * 4 + "xyzw".index(comp)
        defines.append("#define %s_%sDescriptorIndex %d\n" % (name, kind, slot & 15))
    block = INDEX_RE.sub("", block)
    block = block.replace("{", "{\n\tuint4 g_SvrSwzLo[16] : packoffset(c0);\n\tuint4 g_SvrSwzHi[16] : packoffset(c24);"
                          "\n\tuint g_SvrDepthFlip : packoffset(c21.z);", 1)
    text = (head + "".join(defines) + "#define SVR_SWZ_TABLE %s\n" % ("g_SvrSwzHi" if vertex else "g_SvrSwzLo") +
            sep + block + sep2 + rest)
    # The game's constants: one float4[256] per stage. fxc sizes a cbuffer to its highest declared
    # register and reads past it as 0, but the game indexes its arrays beyond their declared
    # length (bone matrices up to c255 through a 63-entry array): skinning and lights read zeros,
    # the scene was almost black. DXC (D3D12) reads the raw buffer, as the Xbox's constant file.
    # D3D11 has no reversed viewport depth range (MinZ > MaxZ): the renderer swaps it and sets
    # g_SvrDepthFlip, and the vertex shader writes z' = w - z, the same depth.
    if vertex:
        line = "\toPos.xy = oPos.xy * g_ScreenSpace.xy + g_ScreenSpace.zw * oPos.w;\n"
        if text.count(line) != 1:
            raise ValueError("vertex shader without its g_ScreenSpace line")
        text = text.replace(line, line + "\tif (g_SvrDepthFlip) oPos.z = oPos.w - oPos.z;\n")
    text = whole_constant_file(text, "VertexShaderConstants", "g_SvrVsC")
    text = whole_constant_file(text, "PixelShaderConstants", "g_SvrPsC")
    # Fetches: the raw ones, then wrapped with the swizzle.
    for f in ("tfetch2D", "tfetch2DBicubic", "tfetch3D", "tfetchCube"):
        text, n = re.subn(r"float4 %s\(" % f, "float4 %sRaw(" % f, text)
        if n != 1:
            raise ValueError("%s defined %d times" % (f, n))
    at = text.find("float4 tfetchR11G11B10(")
    if at < 0:
        raise ValueError("no tfetchR11G11B10")
    text = text[:at] + SWIZZLE + "\n" + text[at:]
    # Spec constants compiled in.
    text = text.replace("uint g_SpecConstants();", "uint g_SpecConstants() { return SVR_SPEC_CONSTANTS; }")
    # fxc's older HLSL (no variadic macros; "texture" is a keyword there).
    text = re.sub(r"\btexture\b", "tex", text)
    text = text.replace("#define SVR_SAMPLE(...) SampleLevel(__VA_ARGS__, 0)", "#define SVR_SAMPLE(S, UV) SampleLevel(S, UV, 0)")
    text = text.replace("#define SVR_SAMPLE(...) Sample(__VA_ARGS__)", "#define SVR_SAMPLE(S, UV) Sample(S, UV)")
    text = re.sub(r"\[\[vk::[^\]]*\]\]", "", text)
    text = re.sub(r", space\d+\)", ")", text)
    text = re.sub(r'\[shader\("\w+"\)\]', "", text)
    text = text.replace("#define DEFINE_SHARED_CONSTANTS() \\", "#define DEFINE_SHARED_CONSTANTS \\")
    text = text.replace("DEFINE_SHARED_CONSTANTS();", "DEFINE_SHARED_CONSTANTS")
    for name in set(ARRAY_MACRO_RE.findall(text)):
        text = re.sub(r"\b%s\[" % re.escape(name), name + "_a[", text)
    text = "#define select(C, A, B) ((C) ? (A) : (B))\n" + text
    text = finite_calls(text)
    start = text.find("void main(")
    if "SvrFinite(" in text and start >= 0:
        text = text[:start] + FINITE_HELPER + text[start:]
    return text


def subsets(mask):
    out, sub = [], mask
    while True:
        out.append(sub)
        if sub == 0:
            return out
        sub = (sub - 1) & mask


def compile_one(job):
    src, stage, mask, work = job
    out = os.path.join(work, "%s.%d.dxbc" % (os.path.basename(src), mask))
    r = subprocess.run([FXC, "/nologo", "/T", stage + "_5_0", "/E", "main", "/O3", "/DREBLUE_RECOMP",
                        "/DSVR_SPEC_CONSTANTS=%d" % mask] + (["/DSVR_VERTEX_SHADER=1"] if stage == "vs" else []) +
                       ["/Fo", out, src], capture_output=True, text=True, timeout=600)
    if r.returncode:
        err = next((l for l in (r.stderr + r.stdout).splitlines() if "error" in l), r.stderr[:300])
        return job, None, err.strip()
    with open(out, "rb") as f:
        return job, f.read(), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hlsl")
    ap.add_argument("out")
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("--keep", help="folder for the converted HLSL and DXBC (else a temporary one)")
    ap.add_argument("--only", help="only these shaders (comma-separated file stems), for tests")
    a = ap.parse_args()
    if not FXC:
        sys.exit("fxc.exe not found (Windows SDK)")
    work = a.keep or tempfile.mkdtemp(prefix="wwe13_dxbc_")
    os.makedirs(work, exist_ok=True)
    jobs, failures = [], []
    names = sorted(glob.glob(os.path.join(a.hlsl, "*.hlsl")))
    if a.only:
        wanted = set(a.only.split(","))
        names = [n for n in names if os.path.basename(n).split(".")[0] in wanted]
    for path in names:
        with open(path, encoding="utf-8", errors="ignore") as f:
            text = f.read()
        m = HEADER_RE.match(text)
        if not m:
            failures.append((path, "no header"))
            continue
        stage, h, mask = "ps" if m.group(1) == "pixel" else "vs", int(m.group(2), 16), int(m.group(3), 16)
        try:
            text = d3d11_source(text, stage == "vs")
        except ValueError as e:
            failures.append((path, str(e)))
            continue
        src = os.path.join(work, os.path.basename(path))
        with open(src, "w", encoding="utf-8") as f:
            f.write(text)
        for sub in subsets(mask):
            jobs.append((src, stage, sub, work, h))
    entries = []
    with concurrent.futures.ThreadPoolExecutor(a.j) as pool:
        for job, (_, blob, err) in zip(jobs, pool.map(lambda j: compile_one(j[:4]), jobs)):
            if err:
                failures.append(("%s s%d" % (os.path.basename(job[0]), job[2]), err))
            else:
                entries.append((job[4], job[2], blob))
    entries.sort(key=lambda e: (e[0], e[1]))
    raw = bytearray()
    index = []
    for h, mask, blob in entries:
        index.append((h, mask, len(raw), len(blob)))
        raw += blob
    packed = zlib.compress(bytes(raw), 9)
    with open(a.out, "w", encoding="utf-8") as f:
        f.write("// Generated by port/tools/convert_shaders_d3d11.py: the game's shaders as DXBC (wwe13_d3d11.exe).\n")
        f.write("// Not versioned: derived from the game's data.\n#include <cstddef>\n#include <cstdint>\n\n")
        f.write("struct Wwe12DxbcEntry { uint64_t hash; uint32_t specConstants; uint32_t offset; uint32_t size; };\n")
        f.write("extern const Wwe12DxbcEntry g_wwe12DxbcEntries[] = {\n")
        for h, mask, off, size in index:
            f.write("    {0x%016XULL, %d, %d, %d},\n" % (h, mask, off, size))
        f.write("};\nextern const size_t g_wwe12DxbcEntryCount = %d;\n" % len(index))
        f.write("extern const size_t g_wwe12DxbcRawSize = %d;\n" % len(raw))
        f.write("extern const size_t g_wwe12DxbcPackedSize = %d;\n" % len(packed))
        f.write("extern const unsigned char g_wwe12DxbcPacked[] = {\n")
        for i in range(0, len(packed), 32):
            f.write("".join("%d," % b for b in packed[i:i + 32]) + "\n")
        f.write("};\n")
    print("%d DXBC variants (%d shaders), %.1f MB -> %.1f MB packed, %d failures" %
          (len(entries), len(names), len(raw) / 1e6, len(packed) / 1e6, len(failures)))
    for name, err in failures[:40]:
        print("  FAILED %s: %s" % (name, err))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
