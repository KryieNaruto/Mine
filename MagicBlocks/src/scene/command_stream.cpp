// MagicBlocks SDK —— L2 命令流:规范化字节序列 / FNV-1a / 二进制编解码 / 稳定 JSON
//
// 见 include/magicblocks/scene/command_stream.h 的设计说明。
//
// **本文件不读时钟**(D13 门控 check_scene_no_clock.py 机械保证),
// **不含任何 Vulkan 类型**(G3)。
#include "magicblocks/scene/command_stream.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace magicblocks::scene {
namespace {

// ---------------------------------------------------------------- 字节读写器

// 逐字段写小端。**禁止**整结构体 memcpy —— 那样会把填充字节也写进流,
// 哈希就随编译器实现漂移了。
class ByteWriter {
 public:
  void put_u8(std::uint8_t v) { buf_.push_back(v); }

  void put_u16(std::uint16_t v) {
    buf_.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
  }

  void put_u32(std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) buf_.push_back(static_cast<std::uint8_t>((v >> s) & 0xFFu));
  }

  void put_u64(std::uint64_t v) {
    for (int s = 0; s < 64; s += 8) buf_.push_back(static_cast<std::uint8_t>((v >> s) & 0xFFu));
  }

  void put_i32(std::int32_t v) { put_u32(static_cast<std::uint32_t>(v)); }
  void put_i64(std::int64_t v) { put_u64(static_cast<std::uint64_t>(v)); }

  // 浮点一律按**位模式**写(bit_cast),这样 -0.0 与 0.0 是不同的字节,
  // NaN 的位模式也原样保留(spec §6.3 / R7:不做 float 比较)。
  void put_f32(float v) { put_u32(std::bit_cast<std::uint32_t>(v)); }
  void put_bool(bool v) { put_u8(v ? 1u : 0u); }

  void put_bytes(const std::uint8_t* data, std::size_t size) {
    buf_.insert(buf_.end(), data, data + size);
  }

  void put_string(const std::string& s) {
    put_u32(static_cast<std::uint32_t>(s.size()));
    put_bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
  }

  std::vector<std::uint8_t>& buf() { return buf_; }
  const std::vector<std::uint8_t>& buf() const { return buf_; }

 private:
  std::vector<std::uint8_t> buf_;
};

// 对称读入器。任何越界即失败 —— 调用方拿到 false 就整体返回 Invalid,
// **不返回半截流**。
class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  bool ok() const { return ok_; }
  std::size_t offset() const { return pos_; }
  std::size_t remaining() const { return ok_ ? size_ - pos_ : 0; }

  bool need(std::size_t n) {
    if (!ok_) return false;
    if (pos_ + n > size_) {
      ok_ = false;
      return false;
    }
    return true;
  }

  std::uint8_t get_u8() {
    if (!need(1)) return 0;
    return data_[pos_++];
  }

  std::uint16_t get_u16() {
    if (!need(2)) return 0;
    std::uint16_t v = static_cast<std::uint16_t>(data_[pos_]) |
                      static_cast<std::uint16_t>(data_[pos_ + 1] << 8);
    pos_ += 2;
    return v;
  }

  std::uint32_t get_u32() {
    if (!need(4)) return 0;
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k) v |= static_cast<std::uint32_t>(data_[pos_ + k]) << (8 * k);
    pos_ += 4;
    return v;
  }

  std::uint64_t get_u64() {
    if (!need(8)) return 0;
    std::uint64_t v = 0;
    for (int k = 0; k < 8; ++k) v |= static_cast<std::uint64_t>(data_[pos_ + k]) << (8 * k);
    pos_ += 8;
    return v;
  }

  std::int32_t get_i32() { return static_cast<std::int32_t>(get_u32()); }
  std::int64_t get_i64() { return static_cast<std::int64_t>(get_u64()); }
  float get_f32() { return std::bit_cast<float>(get_u32()); }
  bool get_bool() { return get_u8() != 0; }

  std::vector<std::uint8_t> get_bytes(std::size_t n) {
    if (!need(n)) return {};
    std::vector<std::uint8_t> out(data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return out;
  }

  std::string get_string() {
    const std::uint32_t n = get_u32();
    if (!need(n)) return {};
    std::string out(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return out;
  }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

// ---------------------------------------------------------------- 字段写入(唯一真源)

// 下面这组 write_* / read_* 是**二进制与 JSON 共用的字段清单**。
// 两处任何一处漏一个字段,哈希就会在往返后不一致 —— 所以都走这里。

void write_transform(ByteWriter& w, const Transform& t) {
  for (float v : t.translation) w.put_f32(v);
  for (float v : t.rotation) w.put_f32(v);
  for (float v : t.scale) w.put_f32(v);
}

Transform read_transform(ByteReader& r) {
  Transform t{};
  for (float& v : t.translation) v = r.get_f32();
  for (float& v : t.rotation) v = r.get_f32();
  for (float& v : t.scale) v = r.get_f32();
  return t;
}

void write_aabb(ByteWriter& w, const Aabb& a) {
  for (float v : a.min) w.put_f32(v);
  for (float v : a.max) w.put_f32(v);
  w.put_bool(a.empty);
}

Aabb read_aabb(ByteReader& r) {
  Aabb a{};
  for (float& v : a.min) v = r.get_f32();
  for (float& v : a.max) v = r.get_f32();
  a.empty = r.get_bool();
  return a;
}

void write_node(ByteWriter& w, const NodeSnapshot& n) {
  w.put_u64(n.id);
  w.put_u64(n.parent);
  w.put_string(n.name);
  write_transform(w, n.transform);
  write_aabb(w, n.local_bounds);
  w.put_bool(n.visible);
}

NodeSnapshot read_node(ByteReader& r) {
  NodeSnapshot n{};
  n.id = r.get_u64();
  n.parent = r.get_u64();
  n.name = r.get_string();
  n.transform = read_transform(r);
  n.local_bounds = read_aabb(r);
  n.visible = r.get_bool();
  return n;
}

void write_event(ByteWriter& w, const EventRecord& e) {
  w.put_u8(static_cast<std::uint8_t>(e.kind));
  w.put_u32(e.pointer_id);
  w.put_f32(e.x);
  w.put_f32(e.y);
  w.put_f32(e.dx);
  w.put_f32(e.dy);
  w.put_i32(e.button);
  w.put_u32(e.width);
  w.put_u32(e.height);
  w.put_i64(e.timestamp_us);
  w.put_bool(e.handled);
}

EventRecord read_event(ByteReader& r) {
  EventRecord e{};
  e.kind = static_cast<EventKind>(r.get_u8());
  e.pointer_id = r.get_u32();
  e.x = r.get_f32();
  e.y = r.get_f32();
  e.dx = r.get_f32();
  e.dy = r.get_f32();
  e.button = r.get_i32();
  e.width = r.get_u32();
  e.height = r.get_u32();
  e.timestamp_us = r.get_i64();
  e.handled = r.get_bool();
  return e;
}

void write_pass(ByteWriter& w, const PassRecord& p) {
  w.put_string(p.name);
  w.put_u8(p.layer);
  w.put_u8(p.channel);
  w.put_bool(p.clear_color);
  for (float v : p.clear_color_rgba) w.put_f32(v);
  w.put_bool(p.clear_depth);
  w.put_f32(p.clear_depth_value);
  for (std::uint32_t v : p.area) w.put_u32(v);
  w.put_u8(static_cast<std::uint8_t>(p.color_format));
  w.put_bool(p.has_depth);
}

PassRecord read_pass(ByteReader& r) {
  PassRecord p{};
  p.name = r.get_string();
  p.layer = r.get_u8();
  p.channel = r.get_u8();
  p.clear_color = r.get_bool();
  for (float& v : p.clear_color_rgba) v = r.get_f32();
  p.clear_depth = r.get_bool();
  p.clear_depth_value = r.get_f32();
  for (std::uint32_t& v : p.area) v = r.get_u32();
  p.color_format = static_cast<Format>(r.get_u8());
  p.has_depth = r.get_bool();
  return p;
}

void write_command(ByteWriter& w, const CommandRecord& c) {
  w.put_u8(static_cast<std::uint8_t>(c.op));
  w.put_u64(c.packed_resource);
  w.put_u64(c.pipeline_hash);
  w.put_u32(c.a);
  w.put_u32(c.b);
  w.put_u32(c.c);
  w.put_i32(c.i);
  for (float v : c.f) w.put_f32(v);
  for (std::uint32_t v : c.pass_area) w.put_u32(v);
  w.put_u32(c.width);
  w.put_u32(c.height);
  w.put_u32(static_cast<std::uint32_t>(c.payload.size()));
  w.put_bytes(c.payload.data(), c.payload.size());
}

CommandRecord read_command(ByteReader& r) {
  CommandRecord c{};
  c.op = static_cast<CommandOp>(r.get_u8());
  c.packed_resource = r.get_u64();
  c.pipeline_hash = r.get_u64();
  c.a = r.get_u32();
  c.b = r.get_u32();
  c.c = r.get_u32();
  c.i = r.get_i32();
  for (float& v : c.f) v = r.get_f32();
  for (std::uint32_t& v : c.pass_area) v = r.get_u32();
  c.width = r.get_u32();
  c.height = r.get_u32();
  const std::uint32_t n = r.get_u32();
  c.payload = r.get_bytes(n);
  return c;
}

// 场景快照的顺序是**契约的一部分**(按 id 升序)。序列化前不重排 ——
// 那样会掩盖"调用方没排序"的 bug;排序由 IScene::snapshot() 保证。
void write_stream_body(ByteWriter& w, const CommandStream& s) {
  // backend 字符串必须进 body(因此进哈希)。二进制头部只放"跨机稳定的枚举值"
  // (0 = Vulkan),但枚举值只有 1 bit 信息,不足以区分 `"vulkan"` 与 `"vulkan2"`;
  // 若只依赖头部,改 backend 名字哈希不变 —— 那是可观测的哈希漏洞。
  w.put_string(s.backend);
  w.put_u64(s.frame_index);
  w.put_u32(s.extent.width);
  w.put_u32(s.extent.height);

  w.put_u64(static_cast<std::uint64_t>(s.events.size()));
  for (const EventRecord& e : s.events) write_event(w, e);

  w.put_u64(static_cast<std::uint64_t>(s.scene.nodes.size()));
  for (const NodeSnapshot& n : s.scene.nodes) write_node(w, n);

  w.put_u64(static_cast<std::uint64_t>(s.passes.size()));
  for (const PassRecord& p : s.passes) write_pass(w, p);

  w.put_u64(static_cast<std::uint64_t>(s.commands.size()));
  for (const CommandRecord& c : s.commands) write_command(w, c);
}

// ---------------------------------------------------------------- JSON 辅助

// 浮点文本:**只用 %.9g**,保证 `float` 的往返无损(float 需要 9 位有效数字)。
std::string f2s(float v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
  return std::string(buf);
}

std::string jstr(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char ch : s) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(ch)));
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
  return out;
}

const char* layer_name(std::uint8_t v) {
  return v == 0 ? "scene" : (v == 1 ? "overlay" : "other");
}

const char* format_name(Format f) {
  switch (f) {
    case Format::RGBA8_UNORM: return "rgba8_unorm";
    case Format::BGRA8_UNORM: return "bgra8_unorm";
    case Format::R8_UNORM: return "r8_unorm";
    case Format::D16_UNORM: return "d16_unorm";
    case Format::D24_UNORM_S8_UINT: return "d24_unorm_s8_uint";
    case Format::D32_SFLOAT: return "d32_sfloat";
    case Format::Unknown: break;
  }
  return "unknown";
}

const char* event_kind_name(EventKind k) {
  switch (k) {
    case EventKind::PointerDown: return "pointer_down";
    case EventKind::PointerUp: return "pointer_up";
    case EventKind::PointerMove: return "pointer_move";
    case EventKind::PointerCancel: return "pointer_cancel";
  }
  return "unknown";
}

}  // namespace

// ---------------------------------------------------------------- 名称

const char* command_op_name(CommandOp op) {
  switch (op) {
    case CommandOp::BindPipeline: return "bind_pipeline";
    case CommandOp::BindVertexBuffer: return "bind_vertex_buffer";
    case CommandOp::BindIndexBuffer: return "bind_index_buffer";
    case CommandOp::Draw: return "draw";
    case CommandOp::DrawIndexed: return "draw_indexed";
    case CommandOp::DrawInstanced: return "draw_instanced";
    case CommandOp::PushConstants: return "push_constants";
    case CommandOp::SetViewport: return "set_viewport";
    case CommandOp::SetScissor: return "set_scissor";
    case CommandOp::BeginPass: return "begin_pass";
    case CommandOp::EndPass: return "end_pass";
  }
  return "unknown";
}

// ---------------------------------------------------------------- 比较(逐字段,浮点比位)

namespace {

bool eq_f(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
bool eq_f3(const float* a, const float* b) { return eq_f(a[0], b[0]) && eq_f(a[1], b[1]) && eq_f(a[2], b[2]); }

}  // namespace

bool Transform::operator==(const Transform& o) const {
  for (int k = 0; k < 3; ++k)
    if (!eq_f(translation[k], o.translation[k])) return false;
  for (int k = 0; k < 4; ++k)
    if (!eq_f(rotation[k], o.rotation[k])) return false;
  for (int k = 0; k < 3; ++k)
    if (!eq_f(scale[k], o.scale[k])) return false;
  return true;
}

bool Aabb::operator==(const Aabb& o) const {
  return eq_f3(min, o.min) && eq_f3(max, o.max) && empty == o.empty;
}

bool NodeSnapshot::operator==(const NodeSnapshot& o) const {
  return id == o.id && parent == o.parent && name == o.name && transform == o.transform &&
         local_bounds == o.local_bounds && visible == o.visible;
}

bool CommandRecord::operator==(const CommandRecord& o) const {
  if (op != o.op || packed_resource != o.packed_resource || pipeline_hash != o.pipeline_hash)
    return false;
  if (a != o.a || b != o.b || c != o.c || i != o.i) return false;
  for (int k = 0; k < 7; ++k)
    if (!eq_f(f[k], o.f[k])) return false;
  for (int k = 0; k < 4; ++k)
    if (pass_area[k] != o.pass_area[k]) return false;
  return width == o.width && height == o.height && payload == o.payload;
}

bool PassRecord::operator==(const PassRecord& o) const {
  if (name != o.name || layer != o.layer || channel != o.channel) return false;
  if (clear_color != o.clear_color || clear_depth != o.clear_depth) return false;
  for (int k = 0; k < 4; ++k)
    if (!eq_f(clear_color_rgba[k], o.clear_color_rgba[k])) return false;
  if (!eq_f(clear_depth_value, o.clear_depth_value)) return false;
  for (int k = 0; k < 4; ++k)
    if (area[k] != o.area[k]) return false;
  return color_format == o.color_format && has_depth == o.has_depth;
}

bool CommandStream::operator==(const CommandStream& o) const {
  if (backend != o.backend || engine_major != o.engine_major) return false;
  if (frame_index != o.frame_index || extent != o.extent) return false;
  if (scene != o.scene || passes != o.passes || commands != o.commands) return false;
  if (events.size() != o.events.size()) return false;
  for (std::size_t k = 0; k < events.size(); ++k) {
    const EventRecord& a = events[k];
    const EventRecord& b = o.events[k];
    if (a.kind != b.kind || a.pointer_id != b.pointer_id || a.button != b.button ||
        a.width != b.width || a.height != b.height || a.timestamp_us != b.timestamp_us ||
        a.handled != b.handled)
      return false;
    if (!eq_f(a.x, b.x) || !eq_f(a.y, b.y) || !eq_f(a.dx, b.dx) || !eq_f(a.dy, b.dy)) return false;
  }
  return true;
}

// ---------------------------------------------------------------- FNV-1a

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size, std::uint64_t seed) {
  std::uint64_t h = seed;
  for (std::size_t k = 0; k < size; ++k) {
    h ^= static_cast<std::uint64_t>(data[k]);
    h *= kFnv1aPrime;
  }
  return h;
}

// ---------------------------------------------------------------- 二进制编码

Result<std::vector<std::uint8_t>> encode_binary(CommandStream& stream) {
  ByteWriter head;
  head.put_bytes(reinterpret_cast<const std::uint8_t*>(kBinaryMagic), 4);
  head.put_u16(kBinaryFormatVersion);
  head.put_u16(0);  // reserved
  // 头部逐字节布局(N8 修正):[8) backend, [9) engine_major, [10..12) reserved u16,
  // 合计 12 字节,之后才是 frame_index。
  head.put_u8(0);                            // backend = 0 → Vulkan(跨机稳定的枚举值)
  head.put_u8(stream.engine_major);
  head.put_u16(0);                           // reserved

  ByteWriter body;
  write_stream_body(body, stream);

  // 先把 body 接在 head 后面算出哈希,再把哈希尾部拼上。
  ByteWriter all;
  all.put_bytes(head.buf().data(), head.buf().size());
  all.put_bytes(body.buf().data(), body.buf().size());

  const std::uint64_t hash = fnv1a64(all.buf().data(), all.buf().size());
  stream.stream_hash = hash;

  all.put_u64(hash);
  return all.buf();
}

Result<CommandStream> decode_binary(const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < kBinaryHeaderBytes + 8) {
    return Status::Invalid("CommandStream::decode: 输入过短(" + std::to_string(bytes.size()) +
                           " 字节,至少需要 " +
                           std::to_string(kBinaryHeaderBytes + 8) + ")");
  }
  ByteReader r(bytes.data(), bytes.size());

  const std::uint8_t* magic = bytes.data();
  for (int k = 0; k < 4; ++k) {
    if (magic[k] != static_cast<std::uint8_t>(kBinaryMagic[k])) {
      return Status::Invalid(std::string("CommandStream::decode: magic 不是 \"MBL2\""));
    }
  }
  r.get_u32();  // 跳过 magic(4)

  const std::uint16_t version = r.get_u16();
  if (version != kBinaryFormatVersion) {
    return Status::Unsupported("CommandStream::decode: 二进制格式版本 " + std::to_string(version) +
                               " 与本进程支持的 " + std::to_string(kBinaryFormatVersion) +
                               " 不一致");
  }
  r.get_u16();                                        // reserved
  const std::uint8_t backend = r.get_u8();
  const std::uint8_t engine_major = r.get_u8();
  r.get_u16();                                        // reserved
  if (backend != 0) {
    return Status::Unsupported("CommandStream::decode: backend 枚举值 " +
                               std::to_string(static_cast<unsigned>(backend)) +
                               " 不支持(本层只认 0 = Vulkan)");
  }
  if (r.offset() != kBinaryHeaderBytes) {
    return Status::Invalid("CommandStream::decode: 头部长度不是 " +
                           std::to_string(kBinaryHeaderBytes) + " 字节");
  }

  CommandStream s{};
  s.engine_major = engine_major;

  // 头部枚举值 0 只表示"这是 Vulkan 流";真正的 backend 名在 body 里,必须原样读回,
  // 否则 `"vulkan"` 与 `"vulkan2"` 解码后会被抹平成同一个值。
  s.backend = r.get_string();
  if (!r.ok()) return Status::Invalid("CommandStream::decode: backend 字符串截断");
  if (backend == 0 && s.backend != "vulkan") {
    return Status::Invalid("CommandStream::decode: 头部 backend 枚举值 0(Vulkan)与 body 里的 \"" +
                           s.backend + "\" 不一致");
  }

  s.frame_index = r.get_u64();
  s.extent.width = r.get_u32();
  s.extent.height = r.get_u32();

  const std::uint64_t event_count = r.get_u64();
  if (!r.ok()) return Status::Invalid("CommandStream::decode: 事件计数处截断");
  if (event_count > r.remaining()) {
    return Status::Invalid("CommandStream::decode: 事件计数 " + std::to_string(event_count) +
                           " 超出剩余 " + std::to_string(r.remaining()) + " 字节");
  }
  s.events.reserve(static_cast<std::size_t>(event_count));
  for (std::uint64_t k = 0; k < event_count; ++k) {
    EventRecord e = read_event(r);
    if (!r.ok()) return Status::Invalid("CommandStream::decode: 第 " + std::to_string(k) + " 个事件截断");
    s.events.push_back(e);
  }

  const std::uint64_t node_count = r.get_u64();
  if (!r.ok()) return Status::Invalid("CommandStream::decode: 节点计数处截断");
  // 每个节点至少 8+8+4+12+16+12+24+1 = 85 字节;用它做廉价的上限保护,
  // 防止恶意/损坏的计数把 reserve 拖爆。
  if (node_count * 85 > r.remaining()) {
    return Status::Invalid("CommandStream::decode: 节点计数 " + std::to_string(node_count) +
                           " 与剩余 " + std::to_string(r.remaining()) + " 字节不匹配");
  }
  s.scene.nodes.reserve(static_cast<std::size_t>(node_count));
  for (std::uint64_t k = 0; k < node_count; ++k) {
    NodeSnapshot n = read_node(r);
    if (!r.ok()) return Status::Invalid("CommandStream::decode: 第 " + std::to_string(k) + " 个节点截断");
    s.scene.nodes.push_back(n);
  }

  const std::uint64_t pass_count = r.get_u64();
  if (!r.ok()) return Status::Invalid("CommandStream::decode: pass 计数处截断");
  if (pass_count * 50 > r.remaining()) {
    return Status::Invalid("CommandStream::decode: pass 计数 " + std::to_string(pass_count) +
                           " 与剩余字节不匹配");
  }
  s.passes.reserve(static_cast<std::size_t>(pass_count));
  for (std::uint64_t k = 0; k < pass_count; ++k) {
    PassRecord p = read_pass(r);
    if (!r.ok()) return Status::Invalid("CommandStream::decode: 第 " + std::to_string(k) + " 个 pass 截断");
    s.passes.push_back(p);
  }

  const std::uint64_t cmd_count = r.get_u64();
  if (!r.ok()) return Status::Invalid("CommandStream::decode: 命令计数处截断");
  // 每条命令固定 1+8+8+4+4+4+4+28+16+4+4+4 = 89 字节 + payload。
  if (cmd_count * 89 > r.remaining()) {
    return Status::Invalid("CommandStream::decode: 命令计数 " + std::to_string(cmd_count) +
                           " 与剩余字节不匹配");
  }
  s.commands.reserve(static_cast<std::size_t>(cmd_count));
  for (std::uint64_t k = 0; k < cmd_count; ++k) {
    CommandRecord c = read_command(r);
    if (!r.ok()) return Status::Invalid("CommandStream::decode: 第 " + std::to_string(k) + " 条命令截断");
    s.commands.push_back(c);
  }

  // 尾部哈希:必须恰好剩 8 字节,且复算要相等。
  const std::uint64_t stored = r.get_u64();
  if (!r.ok()) return Status::Invalid("CommandStream::decode: 尾部哈希截断");
  if (r.remaining() != 0) {
    return Status::Invalid("CommandStream::decode: 尾部多出 " + std::to_string(r.remaining()) +
                           " 字节(流被污染)");
  }
  const std::uint64_t actual = fnv1a64(bytes.data(), bytes.size() - 8);
  if (actual != stored) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "CommandStream::decode: 流哈希不符(记录 0x%016llx,实算 0x%016llx)",
                  static_cast<unsigned long long>(stored),
                  static_cast<unsigned long long>(actual));
    return Status::Invalid(std::string(buf));
  }
  s.stream_hash = stored;
  return s;
}

std::uint64_t compute_stream_hash(const CommandStream& stream) {
  CommandStream copy = stream;
  auto encoded = encode_binary(copy);
  if (!encoded.ok()) return 0;
  return copy.stream_hash;
}

// ---------------------------------------------------------------- JSON

std::string to_json(const CommandStream& stream) {
  std::string out;
  out.reserve(512 + stream.commands.size() * 96 + stream.scene.nodes.size() * 96);

  CommandStream copy = stream;
  const std::uint64_t hash = compute_stream_hash(copy);

  out += "{\n";
  out += "  \"format_version\": " + std::to_string(kBinaryFormatVersion) + ",\n";
  out += "  \"engine_major\": " + std::to_string(static_cast<unsigned>(stream.engine_major)) + ",\n";
  out += "  \"backend\": " + jstr(stream.backend) + ",\n";
  out += "  \"frame_index\": " + std::to_string(stream.frame_index) + ",\n";

  out += "  \"extent\": {\"width\": " + std::to_string(stream.extent.width) +
         ", \"height\": " + std::to_string(stream.extent.height) + "},\n";

  out += "  \"input_events\": [";
  for (std::size_t k = 0; k < stream.events.size(); ++k) {
    const EventRecord& e = stream.events[k];
    out += k == 0 ? "\n" : ",\n";
    out += "    {\"kind\": " + jstr(event_kind_name(e.kind)) +
           ", \"pointer_id\": " + std::to_string(e.pointer_id) +
           ", \"x\": " + f2s(e.x) + ", \"y\": " + f2s(e.y) +
           ", \"dx\": " + f2s(e.dx) + ", \"dy\": " + f2s(e.dy) +
           ", \"button\": " + std::to_string(e.button) +
           ", \"width\": " + std::to_string(e.width) +
           ", \"height\": " + std::to_string(e.height) +
           ", \"timestamp_us\": " + std::to_string(e.timestamp_us) +
           ", \"handled\": " + (e.handled ? "true" : "false") + "}";
  }
  out += stream.events.empty() ? "],\n" : "\n  ],\n";

  out += "  \"scene_snapshot\": {\"nodes\": [";
  for (std::size_t k = 0; k < stream.scene.nodes.size(); ++k) {
    const NodeSnapshot& n = stream.scene.nodes[k];
    out += k == 0 ? "\n" : ",\n";
    out += "    {\"id\": " + std::to_string(n.id) + ", \"parent\": " + std::to_string(n.parent) +
           ", \"name\": " + jstr(n.name) +
           ", \"translation\": [" + f2s(n.transform.translation[0]) + ", " +
           f2s(n.transform.translation[1]) + ", " + f2s(n.transform.translation[2]) +
           "], \"rotation\": [" + f2s(n.transform.rotation[0]) + ", " + f2s(n.transform.rotation[1]) +
           ", " + f2s(n.transform.rotation[2]) + ", " + f2s(n.transform.rotation[3]) +
           "], \"scale\": [" + f2s(n.transform.scale[0]) + ", " + f2s(n.transform.scale[1]) + ", " +
           f2s(n.transform.scale[2]) +
           "], \"local_bounds\": {\"min\": [" + f2s(n.local_bounds.min[0]) + ", " +
           f2s(n.local_bounds.min[1]) + ", " + f2s(n.local_bounds.min[2]) + "], \"max\": [" +
           f2s(n.local_bounds.max[0]) + ", " + f2s(n.local_bounds.max[1]) + ", " +
           f2s(n.local_bounds.max[2]) + "], \"empty\": " + (n.local_bounds.empty ? "true" : "false") +
           "}, \"visible\": " + (n.visible ? "true" : "false") + "}";
  }
  out += stream.scene.nodes.empty() ? "]},\n" : "\n  ]},\n";

  out += "  \"passes\": [";
  for (std::size_t k = 0; k < stream.passes.size(); ++k) {
    const PassRecord& p = stream.passes[k];
    out += k == 0 ? "\n" : ",\n";
    out += "    {\"name\": " + jstr(p.name) + ", \"layer\": " + jstr(layer_name(p.layer)) +
           ", \"channel\": " + std::to_string(p.channel) +
           ", \"clear_color\": " + (p.clear_color ? "true" : "false") + ", \"clear_color_rgba\": [" +
           f2s(p.clear_color_rgba[0]) + ", " + f2s(p.clear_color_rgba[1]) + ", " +
           f2s(p.clear_color_rgba[2]) + ", " + f2s(p.clear_color_rgba[3]) +
           "], \"clear_depth\": " + (p.clear_depth ? "true" : "false") +
           ", \"clear_depth_value\": " + f2s(p.clear_depth_value) + ", \"area\": [" +
           std::to_string(p.area[0]) + ", " + std::to_string(p.area[1]) + ", " +
           std::to_string(p.area[2]) + ", " + std::to_string(p.area[3]) +
           "], \"color_format\": " + jstr(format_name(p.color_format)) +
           ", \"has_depth\": " + (p.has_depth ? "true" : "false") + "}";
  }
  out += stream.passes.empty() ? "],\n" : "\n  ],\n";

  out += "  \"commands\": [";
  for (std::size_t k = 0; k < stream.commands.size(); ++k) {
    const CommandRecord& c = stream.commands[k];
    out += k == 0 ? "\n" : ",\n";
    out += "    {\"op\": " + jstr(command_op_name(c.op)) +
           ", \"resource\": " + std::to_string(c.packed_resource) +
           ", \"pipeline_hash\": " + std::to_string(c.pipeline_hash) +
           ", \"a\": " + std::to_string(c.a) + ", \"b\": " + std::to_string(c.b) +
           ", \"c\": " + std::to_string(c.c) + ", \"i\": " + std::to_string(c.i) +
           ", \"f\": [" + f2s(c.f[0]);
    for (int j = 1; j < 7; ++j) out += ", " + f2s(c.f[j]);
    out += "], \"pass_area\": [" + std::to_string(c.pass_area[0]) + ", " +
           std::to_string(c.pass_area[1]) + ", " + std::to_string(c.pass_area[2]) + ", " +
           std::to_string(c.pass_area[3]) + "], \"width\": " + std::to_string(c.width) +
           ", \"height\": " + std::to_string(c.height) +
           ", \"payload_len\": " + std::to_string(c.payload.size()) + "}";
  }
  out += stream.commands.empty() ? "],\n" : "\n  ],\n";

  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(hash));
    out += "  \"hash\": " + jstr(std::string(buf)) + "\n";
  }
  out += "}\n";
  return out;
}

namespace {

// 极简 JSON 扫描器:只够读回 to_json 的输出。不引入第三方依赖(R6 的精神:
// 运行期不读文件、不加外部库)。任何格式不符都返回错误。
class JsonScanner {
 public:
  explicit JsonScanner(const std::string& t) : t_(t) {}

  void ws() {
    while (pos_ < t_.size()) {
      const char c = t_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool eat(char c) {
    ws();
    if (pos_ < t_.size() && t_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool expect(char c) { return eat(c); }

  bool at_end() {
    ws();
    return pos_ >= t_.size();
  }

  bool peek_is(char c) {
    ws();
    return pos_ < t_.size() && t_[pos_] == c;
  }

  // 读 key(含引号)并吃掉冒号。
  bool key(const char* want) {
    ws();
    if (pos_ >= t_.size() || t_[pos_] != '"') return false;
    ++pos_;
    std::string got;
    if (!raw_string_body(&got)) return false;
    if (got != want) return false;
    return expect(':');
  }

  bool string(std::string* out) {
    ws();
    if (pos_ >= t_.size() || t_[pos_] != '"') return false;
    ++pos_;
    return raw_string_body(out);
  }

  bool number(double* out) {
    ws();
    const std::size_t start = pos_;
    if (pos_ < t_.size() && (t_[pos_] == '-' || t_[pos_] == '+')) ++pos_;
    while (pos_ < t_.size()) {
      const char c = t_[pos_];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
        ++pos_;
      } else {
        break;
      }
    }
    if (pos_ == start) return false;
    try {
      *out = std::stod(t_.substr(start, pos_ - start));
    } catch (...) {
      return false;
    }
    return true;
  }

  // 该位置起的数字是否带小数/指数部分(即不是纯十进制整数)。不移动 pos_。
  bool has_float_tail(std::size_t start) const {
    for (std::size_t k = start; k < t_.size(); ++k) {
      const char c = t_[k];
      if (c == '.' || c == 'e' || c == 'E') return true;
      if (!(c >= '0' && c <= '9')) break;
    }
    return false;
  }

  // 外部工具可能把整数写成 `1e3` / `64.0`;这种形态允许,但要确认转换后仍是精确整数。
  bool number_generic_u64(std::size_t start, std::uint64_t* out) {
    double d = 0.0;
    pos_ = start;
    if (!number(&d)) return false;
    if (d < 0.0) return false;
    if (d != static_cast<double>(static_cast<std::uint64_t>(d))) return false;  // 非整数
    if (d > 1.8e19) return false;                                                // 超出 u64
    *out = static_cast<std::uint64_t>(d);
    return true;
  }

  // 整数必须**按十进制文本精确解析**,绝不能走 `double`:
  // `double` 只有 53 位尾数,`pipeline_hash` / `timestamp_us` 这类 u64/i64 一旦超过
  // 2^53 就会被静默截断(to_json → from_json 哈希不等)。这是
  // `Stream.JsonRoundTripMatchesBinary` 守住的缺陷。
  bool u64(std::uint64_t* out) {
    ws();
    const std::size_t start = pos_;
    if (pos_ < t_.size() && t_[pos_] == '-') return false;  // 负数不是合法 u64
    while (pos_ < t_.size() && t_[pos_] >= '0' && t_[pos_] <= '9') ++pos_;
    // 允许 `1e3` / `1.0` 这类由外部工具写出的整数值:交给 stod 再检查是否为整。
    if (has_float_tail(start)) {
      return number_generic_u64(start, out);
    }
    if (pos_ == start) return false;
    std::uint64_t v = 0;
    for (std::size_t k = start; k < pos_; ++k) {
      const std::uint64_t d = static_cast<std::uint64_t>(t_[k] - '0');
      // 溢出保护:乘 10 或加 d 越界 → 拒绝,而不是静默回绕
      if (v > (std::numeric_limits<std::uint64_t>::max() - d) / 10) return false;
      v = v * 10 + d;
    }
    *out = v;
    return true;
  }

  bool i64(std::int64_t* out) {
    ws();
    bool neg = false;
    if (pos_ < t_.size() && t_[pos_] == '-') {
      neg = true;
      ++pos_;
    }
    std::uint64_t mag = 0;
    if (!u64(&mag)) {
      --pos_;  // 回退负号
      return false;
    }
    if (neg) {
      // 允许 INT64_MIN(幅度 2^63),其余按二进制补码回卷
      if (mag > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ull) {
        return false;
      }
      *out = (mag == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ull)
                 ? std::numeric_limits<std::int64_t>::min()
                 : -static_cast<std::int64_t>(mag);
    } else {
      if (mag > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
      *out = static_cast<std::int64_t>(mag);
    }
    return true;
  }

  bool f32(float* out) {
    double d = 0.0;
    if (!number(&d)) return false;
    *out = static_cast<float>(d);
    return true;
  }

  bool boolean(bool* out) {
    ws();
    if (t_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      *out = true;
      return true;
    }
    if (t_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      *out = false;
      return true;
    }
    return false;
  }

  // 跳过当前值(不关心内容的字段)。
  bool skip_value() {
    ws();
    if (pos_ >= t_.size()) return false;
    const char c = t_[pos_];
    if (c == '"') {
      std::string tmp;
      ++pos_;
      return raw_string_body(&tmp);
    }
    if (c == '{' || c == '[') {
      const char close = (c == '{') ? '}' : ']';
      ++pos_;
      int depth = 1;
      bool in_str = false;
      while (pos_ < t_.size()) {
        const char ch = t_[pos_];
        if (in_str) {
          if (ch == '\\') {
            pos_ += 2;
            continue;
          }
          if (ch == '"') in_str = false;
          ++pos_;
          continue;
        }
        if (ch == '"') {
          in_str = true;
        } else if (ch == c) {
          ++depth;
        } else if (ch == close) {
          --depth;
          if (depth == 0) {
            ++pos_;
            return true;
          }
        }
        ++pos_;
      }
      return false;
    }
    double d = 0.0;
    if (number(&d)) return true;
    bool b = false;
    return boolean(&b);
  }

 private:
  bool raw_string_body(std::string* out) {
    out->clear();
    while (pos_ < t_.size()) {
      const char c = t_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= t_.size()) return false;
      const char esc = t_[pos_++];
      switch (esc) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'u': {
          if (pos_ + 4 > t_.size()) return false;
          const unsigned code =
              static_cast<unsigned>(std::stoul(t_.substr(pos_, 4), nullptr, 16));
          pos_ += 4;
          // 只处理 BMP 里的 ASCII 段(本层的字符串都是 ASCII/UTF-8 原样字节)。
          if (code < 0x80) {
            out->push_back(static_cast<char>(code));
          } else {
            return false;
          }
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  const std::string& t_;
  std::size_t pos_ = 0;
};

EventKind parse_event_kind(const std::string& s) {
  if (s == "pointer_down") return EventKind::PointerDown;
  if (s == "pointer_up") return EventKind::PointerUp;
  if (s == "pointer_cancel") return EventKind::PointerCancel;
  return EventKind::PointerMove;
}

Format parse_format(const std::string& s) {
  if (s == "rgba8_unorm") return Format::RGBA8_UNORM;
  if (s == "bgra8_unorm") return Format::BGRA8_UNORM;
  if (s == "r8_unorm") return Format::R8_UNORM;
  if (s == "d16_unorm") return Format::D16_UNORM;
  if (s == "d24_unorm_s8_uint") return Format::D24_UNORM_S8_UINT;
  if (s == "d32_sfloat") return Format::D32_SFLOAT;
  return Format::Unknown;
}

// op 名字 → 枚举。JSON 往返靠它还原命令类型。
bool parse_op(const std::string& s, CommandOp* out) {
  for (std::uint32_t k = 0; k < kCommandOpCount; ++k) {
    const CommandOp op = static_cast<CommandOp>(k);
    if (s == command_op_name(op)) {
      *out = op;
      return true;
    }
  }
  return false;
}

}  // namespace

Result<CommandStream> from_json(const std::string& text) {
  JsonScanner j(text);
  if (!j.expect('{')) return Status::Invalid("CommandStream::from_json: 顶层不是对象");

  CommandStream s{};
  bool seen_commands = false;

  while (!j.peek_is('}')) {
    std::string k;
    j.ws();
    if (!j.string(&k)) return Status::Invalid("CommandStream::from_json: 缺 key");
    if (!j.expect(':')) return Status::Invalid("CommandStream::from_json: key 后缺冒号");

    if (k == "format_version") {
      std::uint64_t v = 0;
      if (!j.u64(&v)) return Status::Invalid("CommandStream::from_json: format_version 非法");
      if (v != kBinaryFormatVersion) {
        return Status::Unsupported("CommandStream::from_json: 格式版本 " + std::to_string(v) +
                                   " 与本进程支持的 " + std::to_string(kBinaryFormatVersion) +
                                   " 不一致");
      }
    } else if (k == "engine_major") {
      std::uint64_t v = 0;
      if (!j.u64(&v)) return Status::Invalid("CommandStream::from_json: engine_major 非法");
      s.engine_major = static_cast<std::uint8_t>(v);
    } else if (k == "backend") {
      if (!j.string(&s.backend)) return Status::Invalid("CommandStream::from_json: backend 非法");
    } else if (k == "frame_index") {
      if (!j.u64(&s.frame_index)) return Status::Invalid("CommandStream::from_json: frame_index 非法");
    } else if (k == "extent") {
      if (!j.expect('{')) return Status::Invalid("CommandStream::from_json: extent 不是对象");
      while (!j.peek_is('}')) {
        std::string ek;
        if (!j.string(&ek) || !j.expect(':')) return Status::Invalid("CommandStream::from_json: extent 键非法");
        std::uint64_t v = 0;
        if (!j.u64(&v)) return Status::Invalid("CommandStream::from_json: extent 值非法");
        if (ek == "width") {
          s.extent.width = static_cast<std::uint32_t>(v);
        } else if (ek == "height") {
          s.extent.height = static_cast<std::uint32_t>(v);
        }
        j.eat(',');
      }
      if (!j.expect('}')) return Status::Invalid("CommandStream::from_json: extent 未闭合");
    } else if (k == "input_events") {
      if (!j.expect('[')) return Status::Invalid("CommandStream::from_json: input_events 不是数组");
      while (!j.peek_is(']')) {
        if (!j.expect('{')) return Status::Invalid("CommandStream::from_json: 事件不是对象");
        EventRecord e{};
        while (!j.peek_is('}')) {
          std::string ek;
          if (!j.string(&ek) || !j.expect(':')) return Status::Invalid("CommandStream::from_json: 事件键非法");
          bool ok = true;
          if (ek == "kind") {
            std::string v;
            ok = j.string(&v);
            e.kind = parse_event_kind(v);
          } else if (ek == "pointer_id") {
            std::uint64_t v = 0;
            ok = j.u64(&v);
            e.pointer_id = static_cast<std::uint32_t>(v);
          } else if (ek == "x") {
            ok = j.f32(&e.x);
          } else if (ek == "y") {
            ok = j.f32(&e.y);
          } else if (ek == "dx") {
            ok = j.f32(&e.dx);
          } else if (ek == "dy") {
            ok = j.f32(&e.dy);
          } else if (ek == "button") {
            std::int64_t v = 0;
            ok = j.i64(&v);
            e.button = static_cast<std::int32_t>(v);
          } else if (ek == "width") {
            std::uint64_t v = 0;
            ok = j.u64(&v);
            e.width = static_cast<std::uint32_t>(v);
          } else if (ek == "height") {
            std::uint64_t v = 0;
            ok = j.u64(&v);
            e.height = static_cast<std::uint32_t>(v);
          } else if (ek == "timestamp_us") {
            ok = j.i64(&e.timestamp_us);
          } else if (ek == "handled") {
            ok = j.boolean(&e.handled);
          } else {
            ok = j.skip_value();
          }
          if (!ok) return Status::Invalid("CommandStream::from_json: 事件字段 " + ek + " 非法");
          j.eat(',');
        }
        if (!j.expect('}')) return Status::Invalid("CommandStream::from_json: 事件未闭合");
        s.events.push_back(e);
        j.eat(',');
      }
      if (!j.expect(']')) return Status::Invalid("CommandStream::from_json: input_events 未闭合");
    } else if (k == "scene_snapshot") {
      if (!j.expect('{')) return Status::Invalid("CommandStream::from_json: scene_snapshot 不是对象");
      while (!j.peek_is('}')) {
        std::string sk;
        if (!j.string(&sk) || !j.expect(':')) return Status::Invalid("CommandStream::from_json: scene 键非法");
        if (sk != "nodes") {
          if (!j.skip_value()) return Status::Invalid("CommandStream::from_json: scene 字段跳过失败");
          j.eat(',');
          continue;
        }
        if (!j.expect('[')) return Status::Invalid("CommandStream::from_json: nodes 不是数组");
        while (!j.peek_is(']')) {
          if (!j.expect('{')) return Status::Invalid("CommandStream::from_json: 节点不是对象");
          NodeSnapshot n{};
          while (!j.peek_is('}')) {
            std::string nk;
            if (!j.string(&nk) || !j.expect(':')) return Status::Invalid("CommandStream::from_json: 节点键非法");
            bool ok = true;
            if (nk == "id") {
              std::uint64_t v = 0;
              ok = j.u64(&v);
              n.id = v;
            } else if (nk == "parent") {
              std::uint64_t v = 0;
              ok = j.u64(&v);
              n.parent = v;
            } else if (nk == "name") {
              ok = j.string(&n.name);
            } else if (nk == "translation" || nk == "scale") {
              float* dst = (nk == "translation") ? n.transform.translation : n.transform.scale;
              if (!j.expect('[')) return Status::Invalid("CommandStream::from_json: " + nk + " 不是数组");
              for (int c = 0; c < 3; ++c) {
                if (!j.f32(&dst[c])) return Status::Invalid("CommandStream::from_json: " + nk + " 值非法");
                j.eat(',');
              }
              if (!j.expect(']')) return Status::Invalid("CommandStream::from_json: " + nk + " 未闭合");
            } else if (nk == "rotation") {
              if (!j.expect('[')) return Status::Invalid("CommandStream::from_json: rotation 不是数组");
              for (int c = 0; c < 4; ++c) {
                if (!j.f32(&n.transform.rotation[c]))
                  return Status::Invalid("CommandStream::from_json: rotation 值非法");
                j.eat(',');
              }
              if (!j.expect(']')) return Status::Invalid("CommandStream::from_json: rotation 未闭合");
            } else if (nk == "local_bounds") {
              if (!j.expect('{')) return Status::Invalid("CommandStream::from_json: local_bounds 不是对象");
              while (!j.peek_is('}')) {
                std::string bk;
                if (!j.string(&bk) || !j.expect(':')) return Status::Invalid("CommandStream::from_json: bounds 键非法");
                if (bk == "min" || bk == "max") {
                  float* dst = (bk == "min") ? n.local_bounds.min : n.local_bounds.max;
                  if (!j.expect('[')) return Status::Invalid("CommandStream::from_json: bounds 不是数组");
                  for (int c = 0; c < 3; ++c) {
                    if (!j.f32(&dst[c])) return Status::Invalid("CommandStream::from_json: bounds 值非法");
                    j.eat(',');
                  }
                  if (!j.expect(']')) return Status::Invalid("CommandStream::from_json: bounds 未闭合");
                } else if (bk == "empty") {
                  if (!j.boolean(&n.local_bounds.empty))
                    return Status::Invalid("CommandStream::from_json: empty 非法");
                } else if (!j.skip_value()) {
                  return Status::Invalid("CommandStream::from_json: bounds 字段跳过失败");
                }
                j.eat(',');
              }
              if (!j.expect('}')) return Status::Invalid("CommandStream::from_json: local_bounds 未闭合");
            } else if (nk == "visible") {
              ok = j.boolean(&n.visible);
            } else {
              ok = j.skip_value();
            }
            if (!ok) return Status::Invalid("CommandStream::from_json: 节点字段 " + nk + " 非法");
            j.eat(',');
          }
          if (!j.expect('}')) return Status::Invalid("CommandStream::from_json: 节点未闭合");
          s.scene.nodes.push_back(n);
          j.eat(',');
        }
        if (!j.expect(']')) return Status::Invalid("CommandStream::from_json: nodes 未闭合");
        j.eat(',');
      }
      if (!j.expect('}')) return Status::Invalid("CommandStream::from_json: scene_snapshot 未闭合");
    } else if (k == "passes") {
      if (!j.expect('[')) return Status::Invalid("CommandStream::from_json: passes 不是数组");
      while (!j.peek_is(']')) {
        if (!j.expect('{')) return Status::Invalid("CommandStream::from_json: pass 不是对象");
        PassRecord p{};
        while (!j.peek_is('}')) {
          std::string pk;
          if (!j.string(&pk) || !j.expect(':')) return Status::Invalid("CommandStream::from_json: pass 键非法");
          bool ok = true;
          if (pk == "name") {
            ok = j.string(&p.name);
          } else if (pk == "layer") {
            std::string v;
            ok = j.string(&v);
            p.layer = (v == "overlay") ? 1 : 0;
          } else if (pk == "channel") {
            std::uint64_t v = 0;
            ok = j.u64(&v);
            p.channel = static_cast<std::uint8_t>(v);
          } else if (pk == "clear_color") {
            ok = j.boolean(&p.clear_color);
          } else if (pk == "clear_color_rgba") {
            if (!j.expect('[')) return Status::Invalid("CommandStream::from_json: clear_color_rgba 不是数组");
            for (int c = 0; c < 4; ++c) {
              if (!j.f32(&p.clear_color_rgba[c])) return Status::Invalid("CommandStream::from_json: 颜色值非法");
              j.eat(',');
            }
            if (!j.expect(']')) return Status::Invalid("CommandStream::from_json: clear_color_rgba 未闭合");
          } else if (pk == "clear_depth") {
            ok = j.boolean(&p.clear_depth);
          } else if (pk == "clear_depth_value") {
            ok = j.f32(&p.clear_depth_value);
          } else if (pk == "area") {
            if (!j.expect('[')) return Status::Invalid("CommandStream::from_json: area 不是数组");
            for (int c = 0; c < 4; ++c) {
              std::uint64_t v = 0;
              if (!j.u64(&v)) return Status::Invalid("CommandStream::from_json: area 值非法");
              p.area[c] = static_cast<std::uint32_t>(v);
              j.eat(',');
            }
            if (!j.expect(']')) return Status::Invalid("CommandStream::from_json: area 未闭合");
          } else if (pk == "color_format") {
            std::string v;
            ok = j.string(&v);
            p.color_format = parse_format(v);
          } else if (pk == "has_depth") {
            ok = j.boolean(&p.has_depth);
          } else {
            ok = j.skip_value();
          }
          if (!ok) return Status::Invalid("CommandStream::from_json: pass 字段 " + pk + " 非法");
          j.eat(',');
        }
        if (!j.expect('}')) return Status::Invalid("CommandStream::from_json: pass 未闭合");
        s.passes.push_back(p);
        j.eat(',');
      }
      if (!j.expect(']')) return Status::Invalid("CommandStream::from_json: passes 未闭合");
    } else if (k == "commands") {
      if (!j.expect('[')) return Status::Invalid("CommandStream::from_json: commands 不是数组");
      while (!j.peek_is(']')) {
        if (!j.expect('{')) return Status::Invalid("CommandStream::from_json: command 不是对象");
        CommandRecord c{};
        while (!j.peek_is('}')) {
          std::string ck;
          if (!j.string(&ck) || !j.expect(':')) return Status::Invalid("CommandStream::from_json: command 键非法");
          bool ok = true;
          if (ck == "op") {
            std::string v;
            ok = j.string(&v);
            if (ok && !parse_op(v, &c.op)) {
              return Status::Invalid("CommandStream::from_json: 未知命令 op \"" + v + "\"");
            }
          } else if (ck == "resource") {
            ok = j.u64(&c.packed_resource);
          } else if (ck == "pipeline_hash") {
            ok = j.u64(&c.pipeline_hash);
          } else if (ck == "a") {
            std::uint64_t v = 0;
            ok = j.u64(&v);
            c.a = static_cast<std::uint32_t>(v);
          } else if (ck == "b") {
            std::uint64_t v = 0;
            ok = j.u64(&v);
            c.b = static_cast<std::uint32_t>(v);
          } else if (ck == "c") {
            std::uint64_t v = 0;
            ok = j.u64(&v);
            c.c = static_cast<std::uint32_t>(v);
          } else if (ck == "i") {
            std::int64_t v = 0;
            ok = j.i64(&v);
            c.i = static_cast<std::int32_t>(v);
          } else if (ck == "f") {
            if (!j.expect('[')) return Status::Invalid("CommandStream::from_json: f 不是数组");
            for (int x = 0; x < 7; ++x) {
              if (!j.f32(&c.f[x])) return Status::Invalid("CommandStream::from_json: f 值非法");
              j.eat(',');
            }
            if (!j.expect(']')) return Status::Invalid("CommandStream::from_json: f 未闭合");
          } else if (ck == "pass_area") {
            if (!j.expect('[')) return Status::Invalid("CommandStream::from_json: pass_area 不是数组");
            for (int x = 0; x < 4; ++x) {
              std::uint64_t v = 0;
              if (!j.u64(&v)) return Status::Invalid("CommandStream::from_json: pass_area 值非法");
              c.pass_area[x] = static_cast<std::uint32_t>(v);
              j.eat(',');
            }
            if (!j.expect(']')) return Status::Invalid("CommandStream::from_json: pass_area 未闭合");
          } else if (ck == "width") {
            std::uint64_t v = 0;
            ok = j.u64(&v);
            c.width = static_cast<std::uint32_t>(v);
          } else if (ck == "height") {
            std::uint64_t v = 0;
            ok = j.u64(&v);
            c.height = static_cast<std::uint32_t>(v);
          } else if (ck == "payload_len") {
            // JSON 里只记长度;字节本体不进 JSON(二进制才是权威载体)。
            std::uint64_t v = 0;
            ok = j.u64(&v);
            c.payload.assign(static_cast<std::size_t>(v), 0u);
          } else {
            ok = j.skip_value();
          }
          if (!ok) return Status::Invalid("CommandStream::from_json: command 字段 " + ck + " 非法");
          j.eat(',');
        }
        if (!j.expect('}')) return Status::Invalid("CommandStream::from_json: command 未闭合");
        s.commands.push_back(c);
        j.eat(',');
      }
      if (!j.expect(']')) return Status::Invalid("CommandStream::from_json: commands 未闭合");
      seen_commands = true;
    } else {
      if (!j.skip_value()) return Status::Invalid("CommandStream::from_json: 字段 " + k + " 跳过失败");
    }
    j.eat(',');
  }
  if (!j.expect('}')) return Status::Invalid("CommandStream::from_json: 顶层未闭合");
  if (!j.at_end()) return Status::Invalid("CommandStream::from_json: 顶层对象之后有多余内容");
  if (!seen_commands) return Status::Invalid("CommandStream::from_json: 缺 commands 数组");

  s.stream_hash = compute_stream_hash(s);
  return s;
}

}  // namespace magicblocks::scene
