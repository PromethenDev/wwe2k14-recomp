// fxc-lite: the subset of the Windows SDK's fxc.exe that WWE '13 PC's D3D11 build uses
// (convert_shaders_d3d11.py, host_shader_d3d11.py), on Microsoft's own D3DCompiler_47.dll, so a Linux
// cross-build (run under Wine with the genuine DLL) produces the same DXBC as fxc on Windows.
// Options: /nologo /T <profile> /E <entry> /O0-/O3 /D<name>[=<value>] /I <dir> /Fo <file> /Fh <file> /Vn <name> <src>
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <string>
#include <vector>

typedef HRESULT(WINAPI *D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                                      LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static bool ReadFile_(const std::string &path, std::string &out) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}
static std::string DirOf(const std::string &p) {
  const size_t s = p.find_last_of("/\\");
  return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

struct Includer : ID3DInclude {
  std::vector<std::string> dirs;  // source dir first, then /I
  std::vector<std::string> stack;
  HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID *data, UINT *bytes) override {
    std::vector<std::string> search;
    if (!stack.empty()) search.push_back(stack.back());
    search.insert(search.end(), dirs.begin(), dirs.end());
    for (const std::string &d : search) {
      std::string path = d + "/" + name, text;
      if (ReadFile_(path, text)) {
        char *mem = new char[text.size() + 1];
        std::memcpy(mem, text.data(), text.size());
        mem[text.size()] = 0;
        *data = mem;
        *bytes = UINT(text.size());
        stack.push_back(DirOf(path));
        return S_OK;
      }
    }
    return E_FAIL;
  }
  HRESULT __stdcall Close(LPCVOID data) override {
    delete[] static_cast<const char *>(data);
    if (!stack.empty()) stack.pop_back();
    return S_OK;
  }
};

int main(int argc, char **argv) {
  std::string profile, entry = "main", fo, fh, vn = "g_main", src;
  UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL1;
  std::vector<std::string> defs;
  Includer inc;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    // Under Wine the build passes Unix paths (/home/...): an argument with a second '/' is a path.
    const bool is_path = a.size() > 1 && a[0] == '/' && a.find('/', 1) != std::string::npos;
    if (is_path) { src = a; continue; }
    auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "/nologo") continue;
    else if (a == "/T") profile = next();
    else if (a == "/E") entry = next();
    else if (a == "/Fo") fo = next();
    else if (a == "/Fh") fh = next();
    else if (a == "/Vn") vn = next();
    else if (a == "/I") inc.dirs.push_back(next());
    else if (a == "/O0") flags = D3DCOMPILE_OPTIMIZATION_LEVEL0;
    else if (a == "/O1") flags = D3DCOMPILE_OPTIMIZATION_LEVEL1;
    else if (a == "/O2") flags = D3DCOMPILE_OPTIMIZATION_LEVEL2;
    else if (a == "/O3") flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    else if (a.size() > 2 && a.compare(0, 2, "/D") == 0) defs.push_back(a.substr(2));
    else if (!a.empty() && a[0] == '/') { std::fprintf(stderr, "fxc-lite: unsupported option %s\n", a.c_str()); return 2; }
    else src = a;
  }
  if (profile.empty() || src.empty() || (fo.empty() && fh.empty())) {
    std::fprintf(stderr, "fxc-lite: need /T, a source and /Fo or /Fh\n");
    return 2;
  }
  std::string text;
  if (!ReadFile_(src, text)) { std::fprintf(stderr, "fxc-lite: cannot read %s\n", src.c_str()); return 1; }
  inc.dirs.insert(inc.dirs.begin(), DirOf(src));
  std::vector<std::string> names(defs.size()), values(defs.size());
  std::vector<D3D_SHADER_MACRO> macros;
  for (size_t k = 0; k < defs.size(); ++k) {
    const size_t eq = defs[k].find('=');
    names[k] = defs[k].substr(0, eq);
    values[k] = eq == std::string::npos ? "1" : defs[k].substr(eq + 1);
  }
  for (size_t k = 0; k < defs.size(); ++k) macros.push_back({names[k].c_str(), values[k].c_str()});
  macros.push_back({nullptr, nullptr});
  HMODULE dll = LoadLibraryA("D3DCompiler_47.dll");
  auto compile = dll ? reinterpret_cast<D3DCompileFn>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  if (!compile) { std::fprintf(stderr, "fxc-lite: D3DCompiler_47.dll not found\n"); return 1; }
  ID3DBlob *code = nullptr, *errors = nullptr;
  const HRESULT hr = compile(text.data(), text.size(), src.c_str(), macros.data(), &inc, entry.c_str(),
                             profile.c_str(), flags, 0, &code, &errors);
  if (errors) {
    std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    errors->Release();
  }
  if (FAILED(hr) || !code) { std::fprintf(stderr, "fxc-lite: compilation failed (0x%08lX)\n", hr); return 1; }
  const auto *bytes = static_cast<const unsigned char *>(code->GetBufferPointer());
  const size_t size = code->GetBufferSize();
  if (!fo.empty()) {
    FILE *f = std::fopen(fo.c_str(), "wb");
    if (!f) return 1;
    std::fwrite(bytes, 1, size, f);
    std::fclose(f);
  }
  if (!fh.empty()) {
    FILE *f = std::fopen(fh.c_str(), "wb");
    if (!f) return 1;
    std::fprintf(f, "// fxc-lite (D3DCompiler_47.dll) %s %s\n\nconst BYTE %s[] =\n{\n", profile.c_str(), entry.c_str(), vn.c_str());
    for (size_t k = 0; k < size; ++k) std::fprintf(f, "%s%u%s", k % 16 == 0 ? "    " : "", bytes[k], k + 1 < size ? (k % 16 == 15 ? ",\n" : ", ") : "\n");
    std::fprintf(f, "};\n");
    std::fclose(f);
  }
  code->Release();
  return 0;
}
