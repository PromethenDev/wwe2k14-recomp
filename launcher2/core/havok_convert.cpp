#include "havok_convert.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace wwe2k14::launcher {
namespace {

using Bytes = std::vector<uint8_t>;

uint32_t U32(const Bytes& d, size_t o) {
  if (o + 4 > d.size()) throw std::runtime_error("read past the end of a Havok file");
  return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}
int32_t I32(const Bytes& d, size_t o) { return static_cast<int32_t>(U32(d, o)); }
uint16_t U16(const Bytes& d, size_t o) {
  if (o + 2 > d.size()) throw std::runtime_error("read past the end of a Havok file");
  return uint16_t((d[o] << 8) | d[o + 1]);
}
void Put32(Bytes* d, size_t o, uint32_t v) {
  if (o + 4 > d->size()) throw std::runtime_error("write past the end of a converted Havok object");
  (*d)[o] = uint8_t(v >> 24); (*d)[o + 1] = uint8_t(v >> 16); (*d)[o + 2] = uint8_t(v >> 8); (*d)[o + 3] = uint8_t(v);
}
void Append32(Bytes* d, uint32_t v) { d->push_back(uint8_t(v >> 24)); d->push_back(uint8_t(v >> 16)); d->push_back(uint8_t(v >> 8)); d->push_back(uint8_t(v)); }
std::string Field(const Bytes& d, size_t o, size_t n) {
  if (o + n > d.size()) throw std::runtime_error("read past the end of a Havok file");
  std::string v(d.begin() + static_cast<std::ptrdiff_t>(o), d.begin() + static_cast<std::ptrdiff_t>(o + n));
  return v.substr(0, v.find('\0'));
}
std::string CStr(const Bytes& d, size_t o) {
  if (o >= d.size()) throw std::runtime_error("a Havok string is outside the file");
  const auto end = std::find(d.begin() + static_cast<std::ptrdiff_t>(o), d.end(), uint8_t(0));
  if (end == d.end()) throw std::runtime_error("a Havok string has no end");
  return std::string(d.begin() + static_cast<std::ptrdiff_t>(o), end);
}

// hkClassMember::Type (2010-2012 numbering, checked against both games' files)
const char* const kTypes[] = {"VOID", "BOOL", "CHAR", "INT8", "UINT8", "INT16", "UINT16", "INT32", "UINT32", "INT64",
                              "UINT64", "REAL", "VECTOR4", "QUATERNION", "MATRIX3", "ROTATION", "QSTRANSFORM", "MATRIX4",
                              "TRANSFORM", "ZERO", "POINTER", "FUNCTIONPOINTER", "ARRAY", "INPLACEARRAY", "ENUM", "STRUCT",
                              "SIMPLEARRAY", "HOMOGENEOUSARRAY", "VARIANT", "CSTRING", "ULONG", "FLAGS", "HALF", "STRINGPTR",
                              "RELARRAY", "MAX"};
std::string TypeName(uint8_t t) { return t < std::size(kTypes) ? kTypes[t] : "UNKNOWN"; }

constexpr uint16_t kSerializeIgnored = 1024;

struct Member {
  std::string name, type, sub;
  int16_t carr = 0;
  uint16_t flags = 0, offset = 0;
  std::string cls;
};
struct Class {
  std::string parent;
  uint32_t size = 0;
  std::vector<Member> members;
};
struct Section {
  std::string name;
  uint32_t start = 0, lf = 0, gf = 0, vf = 0, ex = 0, im = 0, end = 0;
};
using Loc = std::pair<int, uint32_t>;  // section index, offset

struct Pack {
  Bytes d;
  std::string version;
  uint32_t nsec = 0;
  int32_t contents_sec = 0, contents_off = 0;
  std::vector<Section> secs;
  std::map<std::string, int> by_name;
  std::map<Loc, Loc> ptr;
  std::map<uint32_t, std::string> cn;                     // classnames offset -> name
  std::map<int, std::map<uint32_t, std::string>> objects;  // section -> offset -> class
  std::map<std::string, Class> classes;
  std::map<std::string, std::vector<Member>> flat;

  explicit Pack(const Bytes& bytes) : d(bytes) {
    if (d.size() < 0x40 || U32(d, 0) != 0x57E0E057 || U32(d, 4) != 0x10C0C010) throw std::runtime_error("not a Havok packfile");
    nsec = U32(d, 20);
    if (nsec == 0 || nsec > 8 || 0x40 + 0x30 * nsec > d.size()) throw std::runtime_error("bad Havok section count");
    contents_sec = I32(d, 24); contents_off = I32(d, 28);
    version = Field(d, 40, 16);
    for (uint32_t k = 0; k < nsec; ++k) {
      const size_t o = 0x40 + 0x30 * k;
      Section s;
      s.name = Field(d, o, 19);
      s.start = U32(d, o + 20); s.lf = U32(d, o + 24); s.gf = U32(d, o + 28); s.vf = U32(d, o + 32);
      s.ex = U32(d, o + 36); s.im = U32(d, o + 40); s.end = U32(d, o + 44);
      if (uint64_t(s.start) + s.end > d.size() || s.lf > s.gf || s.gf > s.vf || s.vf > s.ex || s.ex > s.end)
        throw std::runtime_error("bad Havok section table");
      by_name[s.name] = static_cast<int>(k);
      secs.push_back(s);
    }
    for (const char* need : {"__classnames__", "__types__", "__data__"})
      if (!by_name.count(need)) throw std::runtime_error(std::string("Havok file has no ") + need);
    for (int i = 0; i < static_cast<int>(secs.size()); ++i) {
      const Section& s = secs[i];
      for (uint32_t o = s.lf; o + 8 <= s.gf; o += 8) {
        const int32_t a = I32(d, s.start + o), c = I32(d, s.start + o + 4);
        if (a != -1) ptr[{i, uint32_t(a)}] = {i, uint32_t(c)};
      }
      for (uint32_t o = s.gf; o + 12 <= s.vf; o += 12) {
        const int32_t a = I32(d, s.start + o), ds = I32(d, s.start + o + 4), c = I32(d, s.start + o + 8);
        if (a != -1) ptr[{i, uint32_t(a)}] = {ds, uint32_t(c)};
      }
    }
    const Section& c = secs[by_name["__classnames__"]];
    for (uint32_t p = 0; p + 5 <= c.lf;) {
      if (d[c.start + p + 4] != 9) break;
      const std::string name = CStr(d, c.start + p + 5);
      cn[p + 5] = name;
      p += 5 + static_cast<uint32_t>(name.size()) + 1;
    }
    for (int i = 0; i < static_cast<int>(secs.size()); ++i) {
      const Section& s = secs[i];
      for (uint32_t o = s.vf; o + 12 <= s.ex; o += 12) {
        const int32_t a = I32(d, s.start + o), cs = I32(d, s.start + o + 4), off = I32(d, s.start + o + 8);
        (void)cs;
        if (a == -1) continue;
        const auto name = cn.find(uint32_t(off));
        if (name == cn.end()) throw std::runtime_error("Havok object with an unknown class name");
        objects[i][uint32_t(a)] = name->second;
      }
    }
    ReadClasses();
  }
  size_t Abs(int sec, uint32_t off) const {
    if (sec < 0 || size_t(sec) >= secs.size()) throw std::runtime_error("Havok pointer into a missing section");
    return size_t(secs[sec].start) + off;
  }
  std::optional<Loc> Ptr(int sec, uint32_t off) const {
    const auto it = ptr.find({sec, off});
    return it == ptr.end() ? std::nullopt : std::optional<Loc>(it->second);
  }
  std::string StrAt(int sec, uint32_t off) const {
    const auto t = Ptr(sec, off);
    return t ? CStr(d, Abs(t->first, t->second)) : std::string();
  }
  void ReadClasses() {
    const int ti = by_name["__types__"];
    for (const auto& [a, n] : objects[ti]) {
      if (n != "hkClass") continue;
      Class c;
      const std::string name = StrAt(ti, a);
      if (const auto par = Ptr(ti, a + 4)) c.parent = StrAt(par->first, par->second);  // parent hkClass -> its name
      c.size = U32(d, Abs(ti, a + 8));
      const uint32_t count = U32(d, Abs(ti, a + 28));
      if (const auto mt = Ptr(ti, a + 24)) {
        for (uint32_t k = 0; k < count; ++k) {
          const uint32_t mb = mt->second + 24 * k;
          const size_t ab = Abs(mt->first, mb);
          Member m;
          m.name = StrAt(mt->first, mb);
          if (const auto cls = Ptr(mt->first, mb + 4)) m.cls = StrAt(cls->first, cls->second);
          if (ab + 20 > d.size()) throw std::runtime_error("Havok class member outside the file");
          m.type = TypeName(d[ab + 12]); m.sub = TypeName(d[ab + 13]);
          m.carr = static_cast<int16_t>(U16(d, ab + 14)); m.flags = U16(d, ab + 16); m.offset = U16(d, ab + 18);
          c.members.push_back(std::move(m));
        }
      }
      classes[name] = std::move(c);
    }
  }
  const Class& Cls(const std::string& n) const {
    const auto it = classes.find(n);
    if (it == classes.end()) throw std::runtime_error("Havok class " + n + " is not described in the file");
    return it->second;
  }
  const std::vector<Member>& Members(const std::string& n) {
    const auto it = flat.find(n);
    if (it != flat.end()) return it->second;
    std::vector<Member> out;
    const Class& c = Cls(n);
    if (!c.parent.empty()) out = Members(c.parent);
    out.insert(out.end(), c.members.begin(), c.members.end());
    return flat[n] = std::move(out);
  }
  const Member* Find(const std::string& cls, const std::string& name) {
    for (const Member& m : Members(cls)) if (m.name == name) return &m;
    return nullptr;
  }
};

uint32_t ScalarSize(const std::string& t) {
  static const std::map<std::string, uint32_t> s{{"BOOL", 1}, {"CHAR", 1}, {"INT8", 1}, {"UINT8", 1}, {"INT16", 2}, {"UINT16", 2},
      {"INT32", 4}, {"UINT32", 4}, {"INT64", 8}, {"UINT64", 8}, {"REAL", 4}, {"HALF", 2}, {"ULONG", 4}, {"VECTOR4", 16},
      {"QUATERNION", 16}, {"MATRIX3", 48}, {"ROTATION", 48}, {"QSTRANSFORM", 48}, {"MATRIX4", 64}, {"TRANSFORM", 64}};
  const auto it = s.find(t);
  return it == s.end() ? 0 : it->second;
}
uint32_t EnumSize(const std::string& sub) {
  if (sub == "INT8" || sub == "UINT8") return 1;
  if (sub == "INT16" || sub == "UINT16") return 2;
  return 4;
}
uint32_t MemberSize(const Member& m, Pack& p) {
  const uint32_t n = std::max<int>(m.carr, 1);
  if (const uint32_t s = ScalarSize(m.type)) return s * n;
  if (m.type == "ENUM" || m.type == "FLAGS") return EnumSize(m.sub) * n;
  if (m.type == "POINTER" || m.type == "CSTRING" || m.type == "STRINGPTR" || m.type == "FUNCTIONPOINTER") return 4 * n;
  if (m.type == "ARRAY") return 12 * n;
  if (m.type == "SIMPLEARRAY" || m.type == "VARIANT") return 8 * n;
  if (m.type == "STRUCT") return p.Cls(m.cls).size * n;
  if (m.type == "ZERO") return 0;
  throw std::runtime_error("unsupported Havok member type " + m.type);
}
uint32_t ElemSize(const std::string& sub) {
  if (const uint32_t s = ScalarSize(sub)) return s;
  if (sub == "POINTER" || sub == "CSTRING" || sub == "STRINGPTR") return 4;
  if (sub == "INT8" || sub == "UINT8") return 1;
  throw std::runtime_error("unsupported Havok array element " + sub);
}

// documented moves 2011.3 -> 2012.2: target (class, member path) <- source path in the same object
const std::map<std::pair<std::string, std::string>, std::string> kMoves{
    {{"hclSimClothData", "simulationInfo.pinchDetectionEnabled"}, "pinchDetectionEnabled"},
    {{"hclSimClothData", "simulationInfo.landscapeCollisionEnabled"}, "landscapeCollisionEnabled"},
    {{"hclSimClothData", "doNormals"}, "simulationInfo.doNormals"},
};
// gravity: every 2K14 cloth file (0,0,-981,0); WWE '13 (0,0,+981 / +681,0): the 2011 files count it the other way (cape-5).
const std::set<std::pair<std::string, std::string>> kNegate{
    {"hclSimClothSetupObject", "gravity"}, {"hclSimClothDataOverridableSimulationInfo", "gravity"},
    {"hclSimClothDataSimulationInfo", "gravity"}};
const std::set<std::string> kNotBones{"null", "Bip", "Bip Pelvis"};
const std::set<std::string> kExtraBones{"vector"};

struct Writer {
  Pack& src;
  Pack& tpl;
  int ds;
  Bytes data;
  std::vector<std::pair<uint32_t, uint32_t>> local;
  std::vector<std::tuple<uint32_t, int, uint32_t>> glob;
  std::vector<std::pair<uint32_t, uint32_t>> virt;
  std::map<uint32_t, uint32_t> obj_new;
  std::deque<std::pair<uint32_t, Loc>> pending;  // pointer field -> source target object
  std::map<std::string, uint32_t> tcls;          // class name -> template classnames offset
  std::map<std::string, uint32_t> ttypes;        // class name -> template hkClass offset
  std::map<uint32_t, std::string> src_types;     // source hkClass offset -> name
  std::set<std::string> bones;
  bool versions_differ;

  struct Child {
    bool is_string = false;
    uint32_t field = 0;
    Loc target{};
    std::string sub, scls, tcls;
    uint32_t count = 0;
  };

  Writer(Pack& s, Pack& t) : src(s), tpl(t), ds(s.by_name.at("__data__")), versions_differ(s.version != t.version) {
    for (const auto& [off, n] : tpl.cn) tcls[n] = off;
    const int ti = tpl.by_name.at("__types__");
    for (const auto& [a, n] : tpl.objects[ti]) if (n == "hkClass") ttypes[tpl.StrAt(ti, a)] = a;
    const int si = src.by_name.at("__types__");
    for (const auto& [a, n] : src.objects[si]) if (n == "hkClass") src_types[a] = src.StrAt(si, a);
    for (const auto& [a, n] : src.objects[ds]) {
      if (n != "hkaSkeleton") continue;
      const Member* m = src.Find("hkaSkeleton", "bones");
      if (!m) continue;
      const uint32_t cnt = U32(src.d, src.Abs(ds, a + m->offset + 4));
      const auto t = src.Ptr(ds, a + m->offset);
      for (uint32_t k = 0; t && k < cnt; ++k)
        if (const auto q = src.Ptr(t->first, t->second + k * src.Cls(m->cls).size)) bones.insert(CStr(src.d, src.Abs(q->first, q->second)));
    }
  }
  void Align(size_t a) { while (data.size() % a) data.push_back(0); }
  Bytes Raw(int sec, uint32_t off, uint32_t n) {
    const size_t b = src.Abs(sec, off);
    if (b + n > src.d.size()) throw std::runtime_error("Havok member outside the file");
    return Bytes(src.d.begin() + static_cast<std::ptrdiff_t>(b), src.d.begin() + static_cast<std::ptrdiff_t>(b + n));
  }
  void Write(uint32_t at, const Bytes& v) {
    if (size_t(at) + v.size() > data.size()) throw std::runtime_error("Havok member larger than the WWE 2K14 layout");
    std::copy(v.begin(), v.end(), data.begin() + at);
  }
  // An array of count elements of size each must lie inside the source file before room is made for it.
  void CheckFits(const Loc& t, uint64_t size, uint64_t count) const {
    if (size && count > (src.d.size() - std::min(src.d.size(), src.Abs(t.first, t.second))) / size)
      throw std::runtime_error("Havok array larger than the file");
  }

  void Struct(int sec, uint32_t off, const std::string& scls, const std::string& tc, uint32_t base,
              std::vector<Child>& children, const std::string& prefix) {
    std::map<std::string, const Member*> sm;
    for (const Member& m : src.Members(scls)) sm[m.name] = &m;
    for (const Member& tm : tpl.Members(tc)) {
      const std::string path = prefix + tm.name;
      if ((tm.flags & kSerializeIgnored) || tm.type == "ZERO") continue;
      const auto move = kMoves.find({tc, path});
      if (move != kMoves.end()) {
        // a scalar moved within the object
        std::string c = scls; uint32_t o = off; std::string rest = move->second;
        const Member* pm = nullptr;
        while (true) {
          const size_t dot = rest.find('.');
          pm = src.Find(c, rest.substr(0, dot));
          if (!pm || dot == std::string::npos) break;
          c = pm->cls; o += pm->offset; rest = rest.substr(dot + 1);
        }
        if (pm) Write(base + tm.offset, Raw(sec, o + pm->offset, MemberSize(tm, tpl)));
        continue;
      }
      const auto found = sm.find(tm.name);
      if (found == sm.end()) {  // new member: stock values where stock files are not zero
        if (tc == "hkxMaterial" && tm.name == "uvMapScale") { Bytes v(8); Put32(&v, 0, 0x3F800000); Put32(&v, 4, 0x3F800000); Write(base + tm.offset, v); }
        else if (tc == "hkxMaterial" && tm.name == "specularMultiplier") { Bytes v(4); Put32(&v, 0, 0x3F800000); Write(base + tm.offset, v); }
        else if (tc == "hkxMaterial" && tm.name == "specularExponent") { Bytes v(4); Put32(&v, 0, 0x42960000); Write(base + tm.offset, v); }
        else if (tc == "hkxNode" && tm.name == "bone") {
          const auto nm = sm.find("name");
          const std::string node = nm != sm.end() ? src.StrAt(sec, off + nm->second->offset) : std::string();
          if (size_t(base) + tm.offset >= data.size()) throw std::runtime_error("Havok member outside the WWE 2K14 layout");
          data[base + tm.offset] = ((bones.count(node) && !kNotBones.count(node)) || kExtraBones.count(node)) ? 1 : 0;
        }
        continue;
      }
      MemberValue(sec, off, *found->second, tm, tc, base, children, path);
    }
  }

  void MemberValue(int sec, uint32_t off, const Member& m, const Member& tm, const std::string& tc, uint32_t base,
                   std::vector<Child>& children, const std::string& path) {
    if (m.type != tm.type) throw std::runtime_error("Havok member type changed: " + path);
    const uint32_t n = std::max<int>(m.carr, 1);
    if (ScalarSize(m.type) || m.type == "ENUM" || m.type == "FLAGS") {
      Bytes v = Raw(sec, off + m.offset, MemberSize(m, src));
      if (versions_differ && kNegate.count({tc, tm.name}) && v.size() == 16) {
        for (size_t k = 0; k < 16; k += 4) {
          const uint32_t bits = U32(v, k);
          if (bits & 0x7FFFFFFF) Put32(&v, k, bits ^ 0x80000000u);
        }
      }
      Write(base + tm.offset, v);
    } else if (m.type == "STRUCT") {
      for (uint32_t k = 0; k < n; ++k)
        Struct(sec, off + m.offset + k * src.Cls(m.cls).size, m.cls, tm.cls, base + tm.offset + k * tpl.Cls(tm.cls).size, children,
               path + ".");
    } else if (m.type == "POINTER" || m.type == "CSTRING" || m.type == "STRINGPTR") {
      for (uint32_t k = 0; k < n; ++k) {
        const auto t = src.Ptr(sec, off + m.offset + 4 * k);
        if (!t) continue;
        if (m.type == "POINTER" && m.sub == "STRUCT") pending.push_back({base + tm.offset + 4 * k, *t});
        else if (m.type == "POINTER" && t->first != ds) throw std::runtime_error("Havok pointer out of the data section: " + path);
        else { Child c; c.is_string = true; c.field = base + tm.offset + 4 * k; c.target = *t; children.push_back(c); }
      }
    } else if (m.type == "ARRAY" || m.type == "SIMPLEARRAY") {
      const uint32_t fo = off + m.offset;
      const uint32_t cnt = U32(src.d, src.Abs(sec, fo + 4));
      const uint32_t at = base + tm.offset;
      Put32(&data, at, 0); Put32(&data, at + 4, cnt);
      if (m.type == "ARRAY") Put32(&data, at + 8, cnt | 0x80000000u);
      const auto t = src.Ptr(sec, fo);
      if (cnt && t) { Child c; c.field = at; c.target = *t; c.sub = m.sub; c.scls = m.cls; c.tcls = tm.cls; c.count = cnt; children.push_back(c); }
    } else if (m.type == "VARIANT") {
      const uint32_t fo = off + m.offset;
      if (const auto obj = src.Ptr(sec, fo)) pending.push_back({base + tm.offset, *obj});
      if (const auto cl = src.Ptr(sec, fo + 4)) {
        const auto name = src_types.find(cl->second);
        if (name == src_types.end() || !ttypes.count(name->second)) throw std::runtime_error("Havok variant of an unknown class");
        glob.emplace_back(base + tm.offset + 4, tpl.by_name.at("__types__"), ttypes[name->second]);
      }
    } else if (m.type != "FUNCTIONPOINTER") {
      throw std::runtime_error("unsupported Havok member " + path + " (" + m.type + ")");
    }
  }

  void EmitChildren(std::vector<Child>& children) {
    for (size_t i = 0; i < children.size(); ++i) {
      const Child c = children[i];
      if (c.is_string) {
        const size_t sb = src.Abs(c.target.first, c.target.second);
        const std::string s = CStr(src.d, sb);
        Align(2);
        const uint32_t pos = static_cast<uint32_t>(data.size());
        data.insert(data.end(), s.begin(), s.end()); data.push_back(0);
        local.emplace_back(c.field, pos);
        continue;
      }
      Align(16);
      const uint32_t pos = static_cast<uint32_t>(data.size());
      if (c.sub == "STRUCT") {
        const uint32_t ssz = src.Cls(c.scls).size, tsz = tpl.Cls(c.tcls).size;
        CheckFits(c.target, ssz ? ssz : 1, c.count);
        data.resize(data.size() + size_t(tsz) * c.count, 0);
        std::vector<Child> sub;
        for (uint32_t k = 0; k < c.count; ++k) Struct(c.target.first, c.target.second + k * ssz, c.scls, c.tcls, pos + k * tsz, sub, "");
        local.emplace_back(c.field, pos);
        EmitChildren(sub);
      } else if (c.sub == "POINTER") {
        CheckFits(c.target, 4, c.count);
        data.resize(data.size() + 4 * size_t(c.count), 0);
        for (uint32_t k = 0; k < c.count; ++k)
          if (const auto t = src.Ptr(c.target.first, c.target.second + 4 * k)) pending.push_back({pos + 4 * k, *t});
        local.emplace_back(c.field, pos);
      } else if (c.sub == "CSTRING" || c.sub == "STRINGPTR") {
        CheckFits(c.target, 4, c.count);
        data.resize(data.size() + 4 * size_t(c.count), 0);
        local.emplace_back(c.field, pos);
        for (uint32_t k = 0; k < c.count; ++k)
          if (const auto t = src.Ptr(c.target.first, c.target.second + 4 * k)) {
            Child s; s.is_string = true; s.field = pos + 4 * k; s.target = *t; children.push_back(s);
          }
      } else {
        CheckFits(c.target, ElemSize(c.sub), c.count);
        const Bytes v = Raw(c.target.first, c.target.second, static_cast<uint32_t>(ElemSize(c.sub) * uint64_t(c.count)));
        data.insert(data.end(), v.begin(), v.end());
        local.emplace_back(c.field, pos);
      }
    }
    children.clear();
  }

  void Object(uint32_t soff, const std::string& cname) {
    if (obj_new.count(soff)) return;
    if (!tpl.classes.count(cname) || !tcls.count(cname)) throw std::runtime_error("the 2K14 template lacks Havok class " + cname);
    Align(16);
    const uint32_t pos = static_cast<uint32_t>(data.size());
    obj_new[soff] = pos;
    data.resize(data.size() + tpl.Cls(cname).size, 0);
    virt.emplace_back(pos, tcls[cname]);
    std::vector<Child> children;
    Struct(ds, soff, cname, cname, pos, children, "");
    EmitChildren(children);
  }

  Bytes Build() {
    const auto& objs = src.objects[ds];
    const uint32_t root = static_cast<uint32_t>(src.contents_off);
    if (!objs.count(root)) throw std::runtime_error("the Havok root object is missing");
    Object(root, objs.at(root));
    for (const auto& [a, n] : objs) Object(a, n);
    while (!pending.empty()) {
      const auto [field, t] = pending.front();
      pending.pop_front();
      if (t.first != ds || !obj_new.count(t.second)) throw std::runtime_error("a Havok pointer does not point at an object");
      local.emplace_back(field, obj_new[t.second]);
    }
    Align(16);
    if (!tcls.count("hkRootLevelContainer")) throw std::runtime_error("the 2K14 template has no root container class");
    Bytes out(tpl.d.begin(), tpl.d.begin() + 0x40);
    Put32(&out, 28, obj_new[root]);
    Put32(&out, 36, tcls["hkRootLevelContainer"]);
    out.resize(0x40 + 0x30 * tpl.nsec, 0);
    std::vector<std::array<uint32_t, 8>> rows(tpl.nsec);
    const int di = tpl.by_name.at("__data__");
    if (di != static_cast<int>(tpl.nsec) - 1) throw std::runtime_error("the 2K14 template's data section is not last");
    for (int i = 0; i < static_cast<int>(tpl.nsec); ++i) {
      if (i == di) continue;
      const Section& s = tpl.secs[i];
      const uint32_t start = static_cast<uint32_t>(out.size());
      out.insert(out.end(), tpl.d.begin() + s.start, tpl.d.begin() + s.start + s.end);
      rows[i] = {start, s.lf, s.gf, s.vf, s.ex, s.im, s.end, 0};
    }
    const uint32_t start = static_cast<uint32_t>(out.size());
    std::sort(local.begin(), local.end()); std::sort(glob.begin(), glob.end()); std::sort(virt.begin(), virt.end());
    Bytes body = data;
    const uint32_t lf = static_cast<uint32_t>(body.size());
    for (const auto& [a, b] : local) { Append32(&body, a); Append32(&body, b); }
    while ((body.size() - lf) % 16) body.push_back(0xFF);
    const uint32_t gf = static_cast<uint32_t>(body.size());
    for (const auto& [a, s, b] : glob) { Append32(&body, a); Append32(&body, uint32_t(s)); Append32(&body, b); }
    while ((body.size() - gf) % 16) body.push_back(0xFF);
    const uint32_t vf = static_cast<uint32_t>(body.size());
    const uint32_t cni = static_cast<uint32_t>(tpl.by_name.at("__classnames__"));
    for (const auto& [a, b] : virt) { Append32(&body, a); Append32(&body, cni); Append32(&body, b); }
    while ((body.size() - vf) % 16) body.push_back(0xFF);
    const uint32_t end = static_cast<uint32_t>(body.size());
    rows[di] = {start, lf, gf, vf, end, end, end, 0};
    out.insert(out.end(), body.begin(), body.end());
    for (uint32_t i = 0; i < tpl.nsec; ++i) {
      const size_t o = 0x40 + 0x30 * i;
      std::copy(tpl.d.begin() + o, tpl.d.begin() + o + 20, out.begin() + o);
      for (int k = 0; k < 7; ++k) Put32(&out, o + 20 + 4 * k, rows[i][k]);
    }
    return out;
  }
};

}  // namespace

std::string HavokPackfileVersion(const std::vector<uint8_t>& packfile) {
  if (packfile.size() < 0x40 || U32(packfile, 0) != 0x57E0E057 || U32(packfile, 4) != 0x10C0C010) return {};
  std::string v(packfile.begin() + 40, packfile.begin() + 56);
  return v.substr(0, v.find('\0'));
}

bool ConvertHavokPackfile(const std::vector<uint8_t>& source, const std::vector<uint8_t>& layout_template,
                          std::vector<uint8_t>* output, std::string* error) {
  try {
    Pack src(source), tpl(layout_template);
    if (std::memcmp(source.data() + 16, layout_template.data() + 16, 4) != 0)
      throw std::runtime_error("the Havok layout rules of the source and the template differ");
    Writer w(src, tpl);
    *output = w.Build();
    return true;
  } catch (const std::exception& e) {
    if (error) *error = std::string("Havok cloth conversion: ") + e.what();
    return false;
  }
}

}  // namespace wwe2k14::launcher
