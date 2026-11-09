// MagicBlocks L3 —— OBJ 网格加载（spec §13 M4 第 19 项；总 spec P3）
//
// 为什么手写 OBJ 解析而不是直接 tinyobjloader？
// ---------------------------------------------------------------------------
// 试过直接调 tinyobjloader，问题在**它会把两种不同的失败糊成一种**：
//   - 文件语法坏掉（应该报警，资源有问题）
//   - 文件里只有点/线没有面（合法的 OBJ，应该走 fallback）
// 两者的 attrs / shapes 都表现为"空"，调用方分不清。
// 而这个区分对上层是实质性的：前者是资源流水线的 bug，后者是正常情况。
//
// 所以这里手写了一个只覆盖 L3 需要的子集的解析器：
//   v / vn / vt / f（含负索引）/ 注释 / 忽略其它关键字
// 它不到 200 行，每个失败分支都能给出"第几行、什么原因"。
// tinyobjloader 仍然保留在依赖池里（第三方池已构建），
// 将来要支持 mtl/groups/多对象时再接 —— 那时它的信息损失才值得付。
//
// ============================================================================
#include "magicblocks/primitives/mesh_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace magicblocks::primitives {
namespace {

constexpr float kEps = 1e-9f;

// 从顶点列表重算 AABB。
// 这里自己实现一份而不是复用 mesh_factory.cpp 的：那个在匿名 namespace 里，
// 链接期不可见。复制这 15 行比为了共享它而把内部实现提进头文件更划算 ——
// 它是纯函数、无状态，复制不会有"改一处忘一处"的风险。
Box compute_bounds(const std::vector<Vertex>& verts) {
  Box b;
  b.min = Vec3{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
               std::numeric_limits<float>::max()};
  b.max = Vec3{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
               -std::numeric_limits<float>::max()};
  if (verts.empty()) {
    b.min = Vec3{0, 0, 0};
    b.max = Vec3{0, 0, 0};
    return b;
  }
  for (const Vertex& v : verts) {
    for (int k = 0; k < 3; ++k) {
      if (v.position[k] < (&b.min.x)[k]) (&b.min.x)[k] = v.position[k];
      if (v.position[k] > (&b.max.x)[k]) (&b.max.x)[k] = v.position[k];
    }
  }
  return b;
}

// ---------------------------------------------------------------- 小工具
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// 解析一个整数索引。OBJ 的 f 允许负索引（-1 表示"当前最后一个顶点"），
// 所以这里必须接受前导 '-'。而 v/vt/vn 的数组大小不允许为负，
// 那由 resolve_index 的越界检查兜住。
bool parse_index(const char* s, const char* end, long* out) {
  if (s >= end) return false;
  bool neg = false;
  if (*s == '-') {
    neg = true;
    ++s;
  } else if (*s == '+') {
    ++s;
  }
  if (s >= end) return false;
  long v = 0;
  bool any = false;
  while (s < end && *s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    any = true;
    if (v > 2000000000L) return false;  // 防溢出
    ++s;
  }
  if (!any) return false;
  *out = neg ? -v : v;
  return true;
}

bool parse_float(const char* s, const char* end, float* out) {
  // 手写而不是 strtod：strtod 依赖 locale，且需要 NUL 结尾的缓冲区。
  char buf[64];
  std::size_t n = 0;
  while (s < end && n + 1 < sizeof(buf)) buf[n++] = *s++;
  buf[n] = '\0';
  char* e = nullptr;
  const double v = std::strtod(buf, &e);
  if (e == buf) return false;
  *out = static_cast<float>(v);
  return true;
}

// 判断三个点是否共线（退化）
bool collinear(const Vec3& a, const Vec3& b, const Vec3& c) {
  const Vec3 ab{b.x - a.x, b.y - a.y, b.z - a.z};
  const Vec3 ac{c.x - a.x, c.y - a.y, c.z - a.z};
  const Vec3 n{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z,
               ab.x * ac.y - ab.y * ac.x};
  const float len2 = n.x * n.x + n.y * n.y + n.z * n.z;
  return len2 < 1e-16f;
}

// 多边形（给定法线方向）是否为凹多边形。
//
// 为什么要检查：扇式三角化只对凸多边形正确。对凹多边形，
// 扇会把三角形铺到凹口**外面**，得到一个形状错误但语法合法的网格。
// 这种错误在渲染时看不出来（依然是实心的），却和美术预览不一致，
// 属于最难归因的一类问题。宁可明确报 kConcaveFace。
bool is_concave(const std::vector<Vec3>& pts, const Vec3& n) {
  const std::size_t cnt = pts.size();
  if (cnt < 4) return false;

  // 对每条边，看其后所有顶点是否都在同侧。
  // 边方向 e，顶点方向 vd，叉检查的符号即凸/凹。
  float sign = 0.0f;
  for (std::size_t i = 0; i < cnt; ++i) {
    const Vec3& p0 = pts[i];
    const Vec3& p1 = pts[(i + 1) % cnt];
    const Vec3 e{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
    for (std::size_t j = (i + 2) % cnt; j != i; j = (j + 1) % cnt) {
      if (j == (i + 1) % cnt) continue;
      const Vec3& q = pts[j];
      const Vec3 vd{q.x - p0.x, q.y - p0.y, q.z - p0.z};
      const Vec3 cr{e.y * vd.z - e.z * vd.y, e.z * vd.x - e.x * vd.z,
                    e.x * vd.y - e.y * vd.x};
      const float d = cr.x * n.x + cr.y * n.y + cr.z * n.z;
      if (std::fabs(d) < kEps) continue;
      if (sign == 0.0f) {
        sign = d;
      } else if ((sign > 0.0f) != (d > 0.0f)) {
        return true;  // 顶点分布在边的两侧 → 凹
      }
    }
  }
  return false;
}

// 顶点焊接表：把浮点位置量化成整数键做哈希。
//
// 用 bit-exact 哈希会漏掉 DCC 导出的 1e-7 级误差，导致焊接失败；
// 焊不上就变成"每个三角形独立顶点"，法线平均失效，模型看起来全是硬边。
struct Vec3Key {
  std::int64_t x, y, z;
  bool operator==(const Vec3Key& o) const {
    return x == o.x && y == o.y && z == o.z;
  }
};

struct Vec3KeyHash {
  std::size_t operator()(const Vec3Key& k) const {
    // 64 位混合（boost::hash_combine 的变体）
    auto mix = [](std::int64_t v) {
      std::uint64_t u = static_cast<std::uint64_t>(v);
      u ^= u >> 33;
      u *= 0xff51afd7ed558ccdULL;
      u ^= u >> 33;
      return u;
    };
    std::uint64_t h = mix(k.x);
    h = mix(k.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h = mix(k.z) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return static_cast<std::size_t>(h);
  }
};

Vec3Key quantize(const Vec3& p, float inv_eps) {
  return Vec3Key{static_cast<std::int64_t>(std::llround(p.x * inv_eps)),
                 static_cast<std::int64_t>(std::llround(p.y * inv_eps)),
                 static_cast<std::int64_t>(std::llround(p.z * inv_eps))};
}

// ---------------------------------------------------------------- 解析状态
struct ObjRaw {
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<Vec2> uvs;

  // 三角化前的面：每个面存 (pos_idx, normal_idx, uv_idx) 三元组
  struct Corner {
    int pos = -1;  // 已转成 0-based；-1 = 无
    int nrm = -1;
    int uv = -1;
  };
  struct Face {
    std::vector<Corner> corners;
    int line = 0;
    bool smooth = false;  // 该面生成时是否处于平滑组
  };
  std::vector<Face> faces;

  bool saw_any_face = false;
  bool saw_any_geom = false;

  // 平滑组状态。OBJ 的 `s` 指令语义：
  //   `s off` / `s 0` → 硬边（每面独立法线）
  //   `s 1` / `s 2` … → 该组内平滑
  // 缺省（文件里没写 s）按 off 处理 —— 这是 OBJ 规范的做法，
  // 也是更安全的默认：盒子/机械件的硬边不会被神秘地抹圆。
  // 早期这里忽略 s、无条件平滑，结果立方体 8 个角全变成对角法线，
  // 看起来像个球角盒子 —— 顶点数和索引数全对，只有光照是错的。
  bool smoothing_enabled = false;
};

// 把 OBJ 的 1-based / 负索引统一成 0-based。越界返回 false。
bool resolve_index(long raw, std::size_t count, int* out) {
  if (raw == 0) return false;  // OBJ 索引从 1 开始，0 非法
  long idx = raw > 0 ? raw - 1 : static_cast<long>(count) + raw;
  if (idx < 0 || static_cast<std::size_t>(idx) >= count) return false;
  *out = static_cast<int>(idx);
  return true;
}

// 解析 f 的一行：v, v/vt, v//vn, v/vt/vn
bool parse_corner(const char* s, const char* end, const ObjRaw& raw, ObjRaw::Corner* c) {
  const char* p = s;
  // 位置索引（必须）
  const char* seg = p;
  while (p < end && *p != '/' && !is_space(*p)) ++p;
  long pi = 0;
  if (!parse_index(seg, p, &pi)) return false;
  if (!resolve_index(pi, raw.positions.size(), &c->pos)) return false;

  if (p < end && *p == '/') {
    ++p;
    // 可能有 uv（可能为空：v//vn）
    if (p < end && *p != '/' && !is_space(*p)) {
      seg = p;
      while (p < end && *p != '/' && !is_space(*p)) ++p;
      long ti = 0;
      if (!parse_index(seg, p, &ti)) return false;
      if (!resolve_index(ti, raw.uvs.size(), &c->uv)) return false;
    }
    if (p < end && *p == '/') {
      ++p;
      seg = p;
      while (p < end && *p != '/' && !is_space(*p)) ++p;
      long ni = 0;
      if (!parse_index(seg, p, &ni)) return false;
      if (!resolve_index(ni, raw.normals.size(), &c->nrm)) return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------- 主实现
class MeshLoader final : public IMeshLoader {
 public:
  MeshLoadResult load_obj_text(std::string_view text,
                               const MeshLoadOptions& options) override {
    MeshLoadResult res;
    ObjRaw raw;
    MeshLoadError perr = parse(text, options, &raw, &res);
    if (perr != MeshLoadError::kNone) {
      res.error = perr;
      return res;
    }
    MeshLoadResult built = build(raw, options, &res);
    // build() 成功时把 data/stats 都放进 built；失败时 built 里只有
    // error/message/部分 stats。两条路都返回 built，保证调用方拿到的
    // 诊断信息是完整的（失败时 stats 告诉我们解析到哪一步倒下的）。
    if (built.error != MeshLoadError::kNone) {
      built.data.clear();
    }
    return built;
  }

  MeshLoadResult load_obj_file(const std::string& path,
                               const MeshLoadOptions& options) override {
    MeshLoadResult res;

    // 先检查大小再读，避免误加载超大文件。
    // 单独用 ifstream 读进 string 而不是逐行 push_back 到 vector<string>：
    // 后者对大文件分配上百万个小字符串，光分配开销就够慢的。
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      res.error = MeshLoadError::kFileNotFound;
      res.message = "无法打开文件: " + path;
      return res;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) {
      res.error = MeshLoadError::kFileNotFound;
      res.message = "读取失败: " + path;
      return res;
    }
    const std::string text = ss.str();
    return load_obj_text(std::string_view{text}, options);
  }

 private:
  // 第一阶段：文本 → 原始顶点/面
  MeshLoadError parse(std::string_view text, const MeshLoadOptions& options,
                      ObjRaw* raw, MeshLoadResult* res) {
    std::size_t pos = 0;
    int line_no = 0;
    const std::size_t n = text.size();

    while (pos < n) {
      // 取一行（不复制；用指针范围）
      const std::size_t line_start = pos;
      while (pos < n && text[pos] != '\n') ++pos;
      const std::size_t line_end = pos;
      ++line_no;
      if (pos < n) ++pos;  // 跳过 '\n'

      // 去行尾 \r
      std::size_t e = line_end;
      while (e > line_start && (text[e - 1] == '\r' || is_space(text[e - 1]))) --e;
      std::size_t s = line_start;
      while (s < e && is_space(text[s])) ++s;
      if (s >= e || text[s] == '#') continue;

      const char* p = text.data() + s;
      const char* pe = text.data() + e;

      // 关键字 = 首个 token
      const char* kw = p;
      while (p < pe && !is_space(*p)) ++p;
      const std::size_t kwlen = static_cast<std::size_t>(p - kw);
      while (p < pe && is_space(*p)) ++p;

      const auto kw_is = [&](const char* lit) {
        const std::size_t l = std::strlen(lit);
        return kwlen == l && std::memcmp(kw, lit, l) == 0;
      };

      if (kw_is("v")) {
        float v[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) {
          while (p < pe && is_space(*p)) ++p;
          const char* t = p;
          while (p < pe && !is_space(*p)) ++p;
          if (!parse_float(t, p, &v[k])) {
            fail(res, MeshLoadError::kParseFailed, line_no, "顶点坐标不是数字");
            return MeshLoadError::kParseFailed;
          }
        }
        // 第 4 个分量 w（可选，通常 1）忽略
        if (raw->positions.size() >= options.max_vertices) {
          fail(res, MeshLoadError::kTooLarge, line_no, "顶点数超上限");
          return MeshLoadError::kTooLarge;
        }
        raw->positions.push_back(Vec3{v[0], v[1], v[2]});
        raw->saw_any_geom = true;
      } else if (kw_is("vn")) {
        float v[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) {
          while (p < pe && is_space(*p)) ++p;
          const char* t = p;
          while (p < pe && !is_space(*p)) ++p;
          if (!parse_float(t, p, &v[k])) {
            fail(res, MeshLoadError::kParseFailed, line_no, "法线不是数字");
            return MeshLoadError::kParseFailed;
          }
        }
        raw->normals.push_back(Vec3{v[0], v[1], v[2]});
        raw->saw_any_geom = true;
      } else if (kw_is("vt")) {
        float v[2] = {0, 0};
        for (int k = 0; k < 2; ++k) {
          while (p < pe && is_space(*p)) ++p;
          const char* t = p;
          while (p < pe && !is_space(*p)) ++p;
          if (!parse_float(t, p, &v[k])) {
            fail(res, MeshLoadError::kParseFailed, line_no, "uv 不是数字");
            return MeshLoadError::kParseFailed;
          }
        }
        raw->uvs.push_back(Vec2{v[0], v[1]});
        raw->saw_any_geom = true;
      } else if (kw_is("s")) {
        // 平滑组：`s off` / `s 0` → 关；`s <n>` → 开
        const char* t = p;
        const char* te = p;
        while (te < pe && !is_space(*te)) ++te;
        const std::size_t tlen = static_cast<std::size_t>(te - t);
        const bool is_off = (tlen == 3 && std::memcmp(t, "off", 3) == 0) ||
                            (tlen == 1 && *t == '0');
        raw->smoothing_enabled = !is_off;
      } else if (kw_is("f")) {
        ObjRaw::Face face;
        face.line = line_no;
        face.smooth = raw->smoothing_enabled;
        while (p < pe) {
          while (p < pe && is_space(*p)) ++p;
          if (p >= pe) break;
          const char* t = p;
          while (p < pe && !is_space(*p)) ++p;
          ObjRaw::Corner c;
          if (!parse_corner(t, p, *raw, &c)) {
            fail(res, MeshLoadError::kParseFailed, line_no, "面索引非法或越界");
            return MeshLoadError::kParseFailed;
          }
          face.corners.push_back(c);
        }
        if (face.corners.size() < 3) {
          fail(res, MeshLoadError::kParseFailed, line_no, "面少于 3 个顶点");
          return MeshLoadError::kParseFailed;
        }
        raw->faces.push_back(std::move(face));
        raw->saw_any_face = true;
      }
      // 其它关键字（o/g/s/usemtl/mtllib/l/p）忽略 —— L3 不需要分组与材质
    }

    if (!raw->saw_any_face) {
      // 这是**合法**的 OBJ（可能只有点云或线），但与"网格"不符。
      // 用 kEmptyGeometry 而不是 kParseFailed：上层可以据此走 fallback。
      res->error = MeshLoadError::kEmptyGeometry;
      res->message = raw->saw_any_geom ? "文件里没有任何面(f)，只有点/线"
                                       : "文件是空的或不含几何";
      return MeshLoadError::kEmptyGeometry;
    }
    return MeshLoadError::kNone;
  }

  // 第二阶段：原始数据 → 三角化 + 法线
  MeshLoadResult build(const ObjRaw& raw, const MeshLoadOptions& options,
                       MeshLoadResult* res) {
    MeshLoadResult out;
    out.stats.positions = static_cast<std::uint32_t>(raw.positions.size());
    out.stats.normals = static_cast<std::uint32_t>(raw.normals.size());
    out.stats.uvs = static_cast<std::uint32_t>(raw.uvs.size());
    out.stats.faces = static_cast<std::uint32_t>(raw.faces.size());

    MeshData m;
    // 估算容量：面数 × 平均 4 角 × 1.5 三角形
    m.vertices.reserve(raw.faces.size() * 3);
    m.indices.reserve(raw.faces.size() * 6);

    // 每个 (pos, nrm, uv) 组合对应一个输出顶点。
    // 用三元组做键而不是只按 pos 焊接：OBJ 里同一个位置在不同面上
    // 可能有不同法线（硬边），只按位置焊接会把硬边抹平成圆角。
    std::unordered_map<std::int64_t, std::uint32_t> vmap;
    vmap.reserve(raw.faces.size() * 3);

    // 顶点复用键：pos/nrm/uv 各占 21 位。
    //
    // 注意这里**不能**用 std::int64_t 的高位塞面序号 ——
    // pos+1 最大可到 2^21，`<<42` 之后已经占到第 63 位，
    // 再往高位 XOR 一个面号会与位置段冲突，导致键塌缩、
    // 不同面的顶点被错误复用（实测表现为硬边立方体只有 8 个顶点）。
    // 硬边面的面内复用改用独立的 map（见下面 vmap_local）。
    const auto key_of = [](const ObjRaw::Corner& c) -> std::int64_t {
      return (static_cast<std::int64_t>(c.pos + 1) << 42) |
             (static_cast<std::int64_t>(c.nrm + 1) << 21) |
             static_cast<std::int64_t>(c.uv + 1);
    };

    std::vector<Vec3> tri_pos;   // 供法线重算用（三角形顺序与 m.indices 一致）

    // 每个输出顶点是否来自平滑面。法线重算只对平滑顶点做位置焊接 ——
    // 硬边顶点必须保留自己的面法线，不能被邻面平均掉。
    std::vector<bool> vertex_smooth;
    bool any_normal_in_file = false;
    bool any_face_without_normal = false;

    for (const ObjRaw::Face& f : raw.faces) {
      const std::size_t cnt = f.corners.size();

      // 收集这个面的位置（用于法线 / 凹性判定）
      std::vector<Vec3> pts;
      pts.reserve(cnt);
      for (const ObjRaw::Corner& c : f.corners) pts.push_back(raw.positions[c.pos]);

      // 面法线（Newell 方法，对非平面/凹面比单个叉积稳）
      Vec3 fn{0, 0, 0};
      for (std::size_t i = 0; i < cnt; ++i) {
        const Vec3& a = pts[i];
        const Vec3& b = pts[(i + 1) % cnt];
        fn.x += (a.y - b.y) * (a.z + b.z);
        fn.y += (a.z - b.z) * (a.x + b.x);
        fn.z += (a.x - b.x) * (a.y + b.y);
      }
      const float flen = std::sqrt(fn.x * fn.x + fn.y * fn.y + fn.z * fn.z);
      if (flen < kEps) {
        // 退化面（三点共线 / 零面积）：跳过，不算错误。
        // 真实 OBJ 里这种面不少（导出器留下的碎面），报错会让好文件也加载不了。
        continue;
      }
      fn.x /= flen;
      fn.y /= flen;
      fn.z /= flen;

      if (cnt > 3) {
        if (is_concave(pts, fn)) {
          // 凹多边形：扇式三角化会翻面。明确报错而不是产出错误几何。
          out.error = MeshLoadError::kConcaveFace;
          out.message = "第 " + std::to_string(f.line) + " 行的面是凹多边形（L3 只支持凸多边形）";
          out.stats.triangles = static_cast<std::uint32_t>(m.indices.size() / 3);
          return out;
        }
        ++out.stats.polygons_split;
      }

      // 是否有法线
      bool face_has_normal = true;
      for (const ObjRaw::Corner& c : f.corners) {
        if (c.nrm < 0) {
          face_has_normal = false;
          break;
        }
      }
      if (face_has_normal) any_normal_in_file = true;
      else any_face_without_normal = true;

      // 发顶点 + 扇式三角化
      //
      // 顶点复用的判定：
      //   平滑面（s 组内）→ 按 (pos,nrm,uv) 全局复用，法线稍后由邻面平均
      //   硬边面（s off） → 只在**本面内**复用，跨面不复用
      //
      // "硬边面局部复用"是必要的：一个四边形拆成两个三角形后共用一条对角线，
      // 两个三角形共有 2 个顶点。如果完全不复用，12 个三角形的立方体
      // 会变成 36 个顶点（每三角形独立三份），白白多 12 个顶点且破坏
      // 索引缓冲的效率 —— 而 24 才是硬边立方体的正确顶点数。
      // 跨面仍然不能复用，否则邻面法线会被平均，硬边变圆角。
      std::vector<std::uint32_t> local;
      local.reserve(cnt);
      // 硬边面：本面自己的 (位置) → 顶点索引表，实现"面内复用、跨面隔离"。
      // 每个面新建一次，面结束即丢弃 —— 这就是我们要的隔离语义，
      // 而且不需要任何花哨的键编码。
      std::unordered_map<std::int64_t, std::uint32_t> face_local;
      for (const ObjRaw::Corner& c : f.corners) {
        // 平滑面用全局键；硬边面查本面的局部表。
        const std::int64_t gk = key_of(c);
        // ⚠️ 两个 map 的迭代器不能混用 —— 这里曾经把 face_local 的
        // find 结果拿去和 vmap.end() 比较，恒为"未找到"，
        // 于是每个角都重新发顶点……但更糟的是反过来的情况：
        // 一旦真的把 vmap 的迭代器当成了命中，就会返回错误的顶点索引。
        // 正确做法是让两条路各自拿自己的 end()。
        const bool hit = f.smooth ? (vmap.find(gk) != vmap.end())
                                  : (face_local.find(gk) != face_local.end());
        std::uint32_t idx;
        if (hit) {
          idx = f.smooth ? vmap.find(gk)->second : face_local.find(gk)->second;
        } else {
          if (m.vertices.size() >= options.max_vertices) {
            out.error = MeshLoadError::kTooLarge;
            out.message = "顶点数超上限";
            return out;
          }
          Vertex v;
          const Vec3& p = raw.positions[c.pos];
          v.position[0] = p.x;
          v.position[1] = p.y;
          v.position[2] = p.z;
          if (c.nrm >= 0 && c.nrm < static_cast<int>(raw.normals.size())) {
            const Vec3& nn = raw.normals[c.nrm];
            // 归一化：OBJ 的 vn 不保证是单位长度，直接用会让光照强度不稳定
            const float l = std::sqrt(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z);
            if (l > kEps) {
              v.normal[0] = nn.x / l;
              v.normal[1] = nn.y / l;
              v.normal[2] = nn.z / l;
            } else {
              v.normal[0] = fn.x;
              v.normal[1] = fn.y;
              v.normal[2] = fn.z;
            }
          } else {
            // 先用面法线占位；若整体都需要重算，第三阶段会覆盖。
            // 用面法线而不是 (0,1,0)：即使重算被关掉，
            // 结果也至少是"硬边着色"而不是全黑。
            v.normal[0] = fn.x;
            v.normal[1] = fn.y;
            v.normal[2] = fn.z;
          }
          for (int t = 0; t < 2; ++t) v.uv[t] = 0.0f;
          if (c.uv >= 0 && c.uv < static_cast<int>(raw.uvs.size())) {
            v.uv[0] = raw.uvs[c.uv].x;
            v.uv[1] = raw.uvs[c.uv].y;
          }
          set_color(v);
          idx = static_cast<std::uint32_t>(m.vertices.size());
          m.vertices.push_back(v);
          // 平滑顶点进全局表（供跨面复用），硬边顶点进本面局部表。
          if (f.smooth) {
            vmap.emplace(gk, idx);
          } else {
            face_local.emplace(gk, idx);
          }
          vertex_smooth.push_back(f.smooth);
        }
        local.push_back(idx);
      }

      for (std::size_t i = 1; i + 1 < cnt; ++i) {
        if (m.indices.size() + 3 > options.max_indices) {
          out.error = MeshLoadError::kTooLarge;
          out.message = "索引数超上限";
          return out;
        }
        m.indices.push_back(local[0]);
        m.indices.push_back(local[i]);
        m.indices.push_back(local[i + 1]);
      }
    }

    out.stats.triangles = static_cast<std::uint32_t>(m.indices.size() / 3);
    if (m.vertices.empty() || m.indices.empty()) {
      out.error = MeshLoadError::kEmptyGeometry;
      out.message = "所有面都退化（零面积）";
      return out;
    }

    // 第三阶段：法线重算
    //
    // 触发条件：**所有**面都没有法线。部分面有法线的情况不重算，
    // 因为重算会覆盖掉美术精心调的硬边/平滑分界。
    out.stats.normals_synthesized = !any_normal_in_file && options.synthesize_normals;
    out.stats.normals_were_partial = any_normal_in_file && any_face_without_normal;

    if (!any_normal_in_file && options.synthesize_normals) {
      synthesize_normals(&m, options, vertex_smooth);
    }

    m.bounds = compute_bounds(m.vertices);
    out.data = std::move(m);
    res->stats = out.stats;
    return out;
  }

  // 按位置焊接求平滑法线。
  //
  // 关键：焊接**必须**用容差而不是 bit 相等。
  // DCC 工具导出的同一位置常有 1e-7 级别差异，bit 比较焊不上，
  // 结果是每个三角形各自独立 → 平均失效 → 整个模型硬边化。
  static void synthesize_normals(MeshData* m, const MeshLoadOptions& options,
                                 const std::vector<bool>& vertex_smooth) {
    // 用包围盒对角线定容差，避免尺度依赖
    Box b = compute_bounds(m->vertices);
    const float dx = b.max.x - b.min.x;
    const float dy = b.max.y - b.min.y;
    const float dz = b.max.z - b.min.z;
    const float diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float eps = std::max(diag * options.weld_epsilon_ratio, 1e-9f);
    const float inv = 1.0f / eps;

    // 只有平滑顶点参与焊接。硬边顶点的 of[] 指向它自己，
    // 于是它只累加自己所在面的法线 —— 结果就是面法线，正是硬边要的。
    std::unordered_map<Vec3Key, std::uint32_t, Vec3KeyHash> weld;
    weld.reserve(m->vertices.size());

    std::vector<std::uint32_t> of(m->vertices.size());
    for (std::size_t i = 0; i < m->vertices.size(); ++i) {
      const bool smooth = (i < vertex_smooth.size()) && vertex_smooth[i];
      if (!smooth) {
        of[i] = static_cast<std::uint32_t>(i);
        continue;
      }
      const Vec3 p{m->vertices[i].position[0], m->vertices[i].position[1],
                   m->vertices[i].position[2]};
      const Vec3Key k = quantize(p, inv);
      auto it = weld.find(k);
      if (it != weld.end()) {
        of[i] = it->second;
      } else {
        of[i] = static_cast<std::uint32_t>(i);
        weld.emplace(k, static_cast<std::uint32_t>(i));
      }
    }

    // 累加面法线（面积加权：用未归一化的叉积，大三角形影响更大，
    // 这对细分不均的模型更符合视觉预期）
    std::vector<Vec3> acc(m->vertices.size(), Vec3{0, 0, 0});
    for (std::size_t t = 0; t + 2 < m->indices.size(); t += 3) {
      const Vertex& a = m->vertices[m->indices[t + 0]];
      const Vertex& bb = m->vertices[m->indices[t + 1]];
      const Vertex& c = m->vertices[m->indices[t + 2]];
      const Vec3 e1{bb.position[0] - a.position[0], bb.position[1] - a.position[1],
                    bb.position[2] - a.position[2]};
      const Vec3 e2{c.position[0] - a.position[0], c.position[1] - a.position[1],
                    c.position[2] - a.position[2]};
      const Vec3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
                   e1.x * e2.y - e1.y * e2.x};
      for (int k = 0; k < 3; ++k) {
        const std::uint32_t w = of[m->indices[t + k]];
        acc[w].x += n.x;
        acc[w].y += n.y;
        acc[w].z += n.z;
      }
    }

    // 写回
    //
    // ⚠️ 零向量不是"孤立顶点"，而是**硬边顶点**。
    // 立方体的一个角被 3 个互相垂直的面共享，三条面法线之和恰好是
    // 零向量 —— 归一化会失败。最初这里把它当成"孤立顶点"退回 (0,1,0)，
    // 结果是整个立方体 8 个角全被照得像是从上往下打光，
    // 但顶点数、索引数、包围盒全部正确。
    //
    // 正确的处理：零向量说明这个顶点周围的面彼此抵消（典型的盒状硬边），
    // 此时**不应该**有任何平均法线 —— 它本来就该是每个面各自的法线。
    // 所以退回该顶点所属的一个面的法线（面法线在发顶点时已经写进去了，
    // 这里保持不动即可）。
    //
    // 真正"孤立"的顶点（不属于任何三角形）也走同一条路：
    // 它在发顶点阶段拿到的是最后处理的那个面的法线，同样是合法单位向量。
    // 两种情况都不该写零 —— 零法线会让光照全黑且不报错。
    for (std::size_t i = 0; i < m->vertices.size(); ++i) {
      const Vec3& s = acc[of[i]];
      const float l = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);
      if (l > kEps) {
        m->vertices[i].normal[0] = s.x / l;
        m->vertices[i].normal[1] = s.y / l;
        m->vertices[i].normal[2] = s.z / l;
      }
      // else: 保留发顶点阶段写进去的面法线（已是单位向量）
    }
  }

  static void set_color(Vertex& v) {
    v.color[0] = 255;
    v.color[1] = 255;
    v.color[2] = 255;
    v.color[3] = 255;
  }

  static void fail(MeshLoadResult* res, MeshLoadError e, int line, const char* what) {
    res->error = e;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "第 %d 行: %s", line, what);
    res->message = buf;
  }
};

}  // namespace

const char* to_string(MeshLoadError e) {
  switch (e) {
    case MeshLoadError::kNone:
      return "ok";
    case MeshLoadError::kFileNotFound:
      return "file_not_found";
    case MeshLoadError::kParseFailed:
      return "parse_failed";
    case MeshLoadError::kEmptyGeometry:
      return "empty_geometry";
    case MeshLoadError::kNonPlanarFace:
      return "non_planar_face";
    case MeshLoadError::kConcaveFace:
      return "concave_face";
    case MeshLoadError::kTooLarge:
      return "too_large";
  }
  return "unknown";
}

std::unique_ptr<IMeshLoader> create_mesh_loader() {
  return std::make_unique<MeshLoader>();
}

}  // namespace magicblocks::primitives
