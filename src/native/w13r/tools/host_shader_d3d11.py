"""The port's own shaders (copy, gamma, ImGui...) for wwe13_d3d11.exe (from WWE '12 PC): rewritten for fxc, compiled.

The HLSL in port/src/reblue/gpu/shaders/hlsl stays as it is for Vulkan and D3D12; for Direct3D 11
(Shader Model 5.0, no descriptor heaps, no ConstantBuffer<T>, no register spaces) this copy:
- turns each heap `X name[] : register(tN, spaceM)` into `X name[1] : register(tN)`, and every
  index into it into [0]: the renderer binds the texture (sampler) the pass reads to t0 (s0);
- turns `ConstantBuffer<T> name : register(bN, ...)` (push constants) into a cbuffer holding a T;
- drops [[vk::...]] attributes and register spaces; a UAV at u0 moves to u1 (u0 is the render
  target's in a D3D11 pixel shader).

    python -I tools/host_shader_d3d11.py <fxc.exe> <profile> <in.hlsl> <out.h> <variable> [-I dir]
"""
import os
import re
import subprocess
import sys
import tempfile

HEAP_RE = re.compile(r"^(\s*[\w<>, ]+?\s+)(\w+)\[\]\s*:\s*register\((\w+)\s*,\s*space\d+\)\s*;", re.M)
CB_RE = re.compile(r"(?:\[\[vk::push_constant\]\]\s*)?ConstantBuffer<(\w+)>\s+(\w+)\s*:\s*register\((b\d+)[^)]*\)\s*;")


def convert(text):
    heaps = []

    def heap(m):
        heaps.append(m.group(2))
        return "%s%s[1] : register(%s);" % (m.group(1), m.group(2), m.group(3))

    text = HEAP_RE.sub(heap, text)
    text = CB_RE.sub(lambda m: "cbuffer %sBlock : register(%s) { %s %s; };" % (m.group(2), m.group(3), m.group(1), m.group(2)), text)
    text = re.sub(r"\[\[vk::[^\]]*\]\]", "", text)
    text = re.sub(r"register\((\w+)\s*,\s*space\d+\)", r"register(\1)", text)
    # (a pixel shader's UAVs follow its render targets: u0 is taken by SV_Target)
    text = text.replace("register(u0)", "register(u1)")
    for name in heaps:
        out, pos = [], 0
        for m in re.finditer(r"\b%s\[" % re.escape(name), text):
            if m.start() < pos:
                continue
            depth, k = 1, m.end()
            while k < len(text) and depth:
                depth += {"[": 1, "]": -1}.get(text[k], 0)
                k += 1
            if text[m.end():k - 1].strip() in ("1",) and text[k:k + 3].strip().startswith(":"):
                continue  # (the declaration)
            out.append(text[pos:m.end()] + "0]")
            pos = k
        out.append(text[pos:])
        text = "".join(out)
    return text


def main():
    fxc, profile, src, out, var = sys.argv[1:6]
    includes = []
    rest = sys.argv[6:]
    while rest:
        if rest[0] == "-I" and len(rest) > 1:
            includes += ["/I", rest[1]]
            rest = rest[2:]
        else:
            rest = rest[1:]
    src_dir = os.path.dirname(os.path.abspath(src))
    with tempfile.TemporaryDirectory(prefix="wwe13_host_d3d11_") as work:
        # The includes are converted too, next to the shader.
        for name in os.listdir(src_dir):
            if name.endswith((".hlsl", ".hlsli")):
                with open(os.path.join(src_dir, name), encoding="utf-8") as f:
                    text = convert(f.read())
                with open(os.path.join(work, name), "w", encoding="utf-8") as f:
                    f.write(text)
        r = subprocess.run([fxc, "/nologo", "/T", profile, "/E", "main", "/O3", "/DSVR_D3D11=1",
                            "/Fh", out, "/Vn", var] + includes +
                           [os.path.join(work, os.path.basename(src))], capture_output=True, text=True)
        if r.returncode:
            sys.stderr.write(r.stdout + r.stderr)
            return r.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
