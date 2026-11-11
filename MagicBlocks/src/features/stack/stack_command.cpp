// F-VX 命令流录制 / 回放 / 复现 —— 实现（设计 §13.3，计划阶段 7）
//
// 实现顺序刻意与"从真源倒推"相反：这里**先**定义序列化格式，再让录制器
// 往这个格式上写。理由：序列化格式是录制态唯一的对外表示，先定它能让
// "回放必须原样恢复 stroke_id"变成格式的硬约束，而不是靠实现记得。
#include "mb_internal/magicblocks/features/stack/stack_command.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace magicblocks::features::stack {

const char* event_kind_name(EventKind k) {
  switch (k) {
    case EventKind::Stroke: return "stroke";
    case EventKind::Undo: return "undo";
    case EventKind::SetColor: return "set_color";
    case EventKind::LockLayer: return "lock_layer";
    case EventKind::UnlockLayer: return "unlock_layer";
  }
  return "?";
}

namespace {

bool kind_from_name(const std::string& s, EventKind* out) {
  if (s == "stroke") { *out = EventKind::Stroke; return true; }
  if (s == "undo") { *out = EventKind::Undo; return true; }
  if (s == "set_color") { *out = EventKind::SetColor; return true; }
  if (s == "lock_layer") { *out = EventKind::LockLayer; return true; }
  if (s == "unlock_layer") { *out = EventKind::UnlockLayer; return true; }
  return false;
}

StackError err(StackCode c, std::string d) { return StackError::Fail(c, std::move(d)); }

// 事件内容哈希。字段顺序固定，**不**包含任何运行期量（地址/时间/指针）。
void hash_field(std::uint64_t* h, const void* p, std::size_t n) {
  *h = hash_bytes(*h, p, n);
}

std::uint64_t hash_event(std::uint64_t h, const EventRecord& e) {
  const std::uint8_t k = static_cast<std::uint8_t>(e.kind);
  hash_field(&h, &k, sizeof(k));
  hash_field(&h, &e.stroke_id, sizeof(e.stroke_id));
  hash_field(&h, &e.color, sizeof(e.color));
  hash_field(&h, &e.layer, sizeof(e.layer));
  const std::int32_t cx[3] = {e.cell.x, e.cell.y, e.cell.z};
  hash_field(&h, cx, sizeof(cx));
  const std::uint32_t n = static_cast<std::uint32_t>(e.path.size());
  hash_field(&h, &n, sizeof(n));
  for (const PathPoint& p : e.path) {
    const std::int32_t xy[2] = {p.px, p.py};
    hash_field(&h, xy, sizeof(xy));
  }
  return h;
}

// ---- ICommandStream ----
//
// 命名 CommandStream 而不是 CommandStreamImpl：本文件不导出它，
// 但 gtest 需要能构造来测序列化 —— 通过 create_command_stream() 拿。
class CommandStream : public ICommandStream {
 public:
  std::size_t append(const EventRecord& e) override {
    events_.push_back(e);
    return events_.size() - 1;
  }
  std::size_t size() const override { return events_.size(); }
  const EventRecord& at(std::size_t i) const override { return events_[i]; }
  const std::vector<EventRecord>& events() const override { return events_; }
  void clear() override { events_.clear(); }

  std::uint64_t hash() const override {
    std::uint64_t h = kHashSeed;
    const std::uint64_t n = events_.size();
    hash_field(&h, &n, sizeof(n));
    for (const EventRecord& e : events_) h = hash_event(h, e);
    return h;
  }

  // 格式（每行一条事件，字段以空格分隔，定长可 diff）：
  //   stroke <id> <color> <layer> <npts> <px0> <py0> <px1> <py1> ...
  //   undo <id>
  //   set_color <x> <y> <z> <color>
  //   lock_layer <layer>
  //   unlock_layer
  //
  // **路径点用整数**：这是 M10 的前提。浮点在这里会引入"存盘-读回"的
  // 舍入差异，而那正是三态比对要排除的东西。
  std::string serialize() const override {
    std::string out;
    char buf[64];
    for (const EventRecord& e : events_) {
      out += event_kind_name(e.kind);
      switch (e.kind) {
        case EventKind::Stroke:
          std::snprintf(buf, sizeof(buf), " %u %u %d %zu", e.stroke_id, e.color, e.layer,
                        e.path.size());
          out += buf;
          for (const PathPoint& p : e.path) {
            std::snprintf(buf, sizeof(buf), " %d %d", p.px, p.py);
            out += buf;
          }
          break;
        case EventKind::Undo:
          std::snprintf(buf, sizeof(buf), " %u", e.stroke_id);
          out += buf;
          break;
        case EventKind::SetColor:
          std::snprintf(buf, sizeof(buf), " %d %d %d %u", e.cell.x, e.cell.y, e.cell.z, e.color);
          out += buf;
          break;
        case EventKind::LockLayer:
          std::snprintf(buf, sizeof(buf), " %d", e.layer);
          out += buf;
          break;
        case EventKind::UnlockLayer:
          break;
      }
      out += '\n';
    }
    return out;
  }

  bool parse(const std::string& text, std::string* detail) override {
    events_.clear();
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
      std::size_t eol = text.find('\n', pos);
      if (eol == std::string::npos) eol = text.size();
      std::string line = text.substr(pos, eol - pos);
      pos = eol + 1;
      ++line_no;
      if (line.empty()) continue;

      // 切词（单空格分隔，容忍行尾空白）。
      std::vector<std::string> tok;
      std::size_t i = 0;
      while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        const std::size_t b = i;
        while (i < line.size() && line[i] != ' ') ++i;
        if (i > b) tok.push_back(line.substr(b, i - b));
      }
      if (tok.empty()) continue;

      EventRecord e;
      if (!kind_from_name(tok[0], &e.kind)) {
        if (detail) *detail = "第 " + std::to_string(line_no) + " 行: 未知事件种类 '" + tok[0] + "'";
        return false;
      }
      const auto need = [&](std::size_t n) { return tok.size() >= n; };
      const auto num = [](const std::string& s, long* out) {
        char* end = nullptr;
        const long v = std::strtol(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != '\0') return false;
        *out = v;
        return true;
      };
      // 颜色是 uint32，序列化用 %u 打印十进制。**不能**用上面的 strtol 读：
      // 0xAABBCCDD = 2864434397 > INT32_MAX，32 位 long 上 strtol 会 ERANGE
      // 截成 LONG_MAX。这正是 v1 里"序列化往返后哈希变化"的直接原因。
      const auto num_u32 = [](const std::string& s, std::uint32_t* out) {
        if (s.empty() || s[0] == '-') return false;
        char* end = nullptr;
        errno = 0;
        const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != '\0' || errno == ERANGE) return false;
        if (v > 0xFFFFFFFFull) return false;
        *out = static_cast<std::uint32_t>(v);
        return true;
      };
      long v = 0;
      switch (e.kind) {
        case EventKind::Stroke: {
          if (!need(5)) { if (detail) *detail = "第 " + std::to_string(line_no) + " 行: stroke 字段不足"; return false; }
          if (!num(tok[1], &v)) { if (detail) *detail = "第 " + std::to_string(line_no) + " 行: stroke_id 非法"; return false; }
          e.stroke_id = static_cast<std::uint32_t>(v);
          if (!num_u32(tok[2], &e.color)) { if (detail) *detail = "第 " + std::to_string(line_no) + " 行: color 非法"; return false; }
          if (!num(tok[3], &v)) { if (detail) *detail = "第 " + std::to_string(line_no) + " 行: layer 非法"; return false; }
          e.layer = static_cast<std::int32_t>(v);
          long npts = 0;
          if (!num(tok[4], &npts) || npts < 0) { if (detail) *detail = "第 " + std::to_string(line_no) + " 行: 路径点数非法"; return false; }
          if (tok.size() != 5 + static_cast<std::size_t>(npts) * 2) {
            if (detail) *detail = "第 " + std::to_string(line_no) + " 行: 路径点数与实际坐标数不符";
            return false;
          }
          e.path.reserve(static_cast<std::size_t>(npts));
          for (long k = 0; k < npts; ++k) {
            long x = 0, y = 0;
            if (!num(tok[5 + k * 2], &x) || !num(tok[6 + k * 2], &y)) {
              if (detail) *detail = "第 " + std::to_string(line_no) + " 行: 路径坐标非法";
              return false;
            }
            e.path.push_back(PathPoint{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)});
          }
          break;
        }
        case EventKind::Undo:
          if (!need(2) || !num(tok[1], &v)) { if (detail) *detail = "第 " + std::to_string(line_no) + " 行: undo 字段非法"; return false; }
          e.stroke_id = static_cast<std::uint32_t>(v);
          break;
        case EventKind::SetColor: {
          if (!need(5)) { if (detail) *detail = "第 " + std::to_string(line_no) + " 行: set_color 字段不足"; return false; }
          long a[3] = {0, 0, 0};
          for (int k = 0; k < 3; ++k) {
            if (!num(tok[1 + k], &a[k])) { if (detail) *detail = "第 " + std::to_string(line_no) + " 行: set_color 坐标非法"; return false; }
          }
          e.cell = Cell{static_cast<std::int32_t>(a[0]), static_cast<std::int32_t>(a[1]),
                        static_cast<std::int32_t>(a[2])};
          if (!num_u32(tok[4], &e.color)) { if (detail) *detail = "第 " + std::to_string(line_no) + " 行: set_color color 非法"; return false; }
          break;
        }
        case EventKind::LockLayer:
          if (!need(2) || !num(tok[1], &v)) { if (detail) *detail = "第 " + std::to_string(line_no) + " 行: lock_layer 字段非法"; return false; }
          e.layer = static_cast<std::int32_t>(v);
          break;
        case EventKind::UnlockLayer:
          break;
      }
      events_.push_back(std::move(e));
    }
    return true;
  }

 private:
  std::vector<EventRecord> events_;
};

// ---- IRecorder ----
class Recorder : public IRecorder {
 public:
  Recorder() = default;

  StackStatus attach(IBlockSet* set, const GridSpec& grid) override {
    if (set == nullptr) return StackStatus::Fail(StackCode::OutOfGrid, "attach: set 为空");
    set_ = set;
    grid_ = grid;
    brush_ = create_block_brush(BrushConfig{grid_, 1});
    if (!brush_) return StackStatus::Fail(StackCode::OutOfGrid, "attach: 笔刷创建失败");
    return StackStatus::Ok_();
  }

  IBlockSet* set() const override { return set_; }
  ICommandStream& stream() override { return stream_; }
  const ICommandStream& stream() const override { return stream_; }

  StackResult<std::uint32_t> stroke(const std::vector<PathPoint>& path, std::uint32_t rgba,
                                    std::int32_t layer) override {
    if (set_ == nullptr) return err(StackCode::OutOfGrid, "recorder 未 attach");
    if (path.empty()) return err(StackCode::EmptyStroke, "落笔路径为空");

    // 层锁：`layer >= 0` 时本条笔迹锁层；否则按"由射线决定"处理，
    // 而射线在这里是直接由路径点构造的（我们不在这一层做拾取）。
    if (layer >= 0) {
      if (auto s = brush_->layer_lock().lock(layer); !s.ok()) return s.error();
    } else {
      brush_->layer_lock().unlock();
    }

    // 路径点 → 射线。与 CLI 同构：从该层平面上方垂直向下打。
    // 未锁层时落到 layers-1（这是 L3 笔刷的既有语义，见 block_brush.cpp
    // 的 pick_layer_for_ray）；调用方要精确落层就显式给 layer。
    const std::int32_t eff_layer = (layer >= 0) ? layer : (grid_.layers - 1);
    const float plane_y =
        grid_.origin[1] + (static_cast<float>(eff_layer) + 0.5f) * grid_.cell_size;

    std::vector<Ray> rays;
    rays.reserve(path.size());
    for (const PathPoint& p : path) {
      // 像素 → 世界：像素坐标按 cell_size 折算到网格平面。这样测试可以用
      // 整数像素写脚本，而不必手算世界坐标。
      const float wx = static_cast<float>(p.px) * grid_.cell_size;
      const float wz = static_cast<float>(p.py) * grid_.cell_size;
      rays.push_back(Ray{Vec3{wx, plane_y + 8.0f, wz}, Vec3{0.0f, -1.0f, 0.0f}});
    }

    auto plan_r = brush_->stroke(rays.data(), rays.size());
    if (!plan_r.ok()) return plan_r.error();
    const StrokePlan plan = plan_r.value();
    if (plan.cells.empty()) return err(StackCode::EmptyStroke, "落笔没有产生任何格");

    // **id 在这里分配并钉进事件**（M10）。此后回放只看事件里的值。
    const std::uint32_t id = set_->next_stroke_id();

    // 已占格过滤：批量堆叠里后画的圈压在前画的圈上是正常形态，
    // 而 IBlockSet::add_stroke 是"有一格冲突就整体失败"。
    // 所以这里先滤掉已占格，再一次性 add_stroke。
    std::vector<Cell> fresh;
    fresh.reserve(plan.cells.size());
    for (const Cell& c : plan.cells) {
      if (set_->find(c) == nullptr) fresh.push_back(c);
    }
    if (fresh.empty()) {
      // 整笔都被占了：仍记一条 stroke 事件（回放时同样什么都不做）。
      // 这样"流长度"与"用户操作次数"一一对应，回放态的 id 序列也一致。
      EventRecord e;
      e.kind = EventKind::Stroke;
      e.stroke_id = id;
      e.color = rgba;
      e.layer = layer;
      e.path = path;
      stream_.append(e);
      return id;
    }

    if (auto s = add_stroke(set_, fresh, rgba, id); !s.ok()) return s.error();

    EventRecord e;
    e.kind = EventKind::Stroke;
    e.stroke_id = id;
    e.color = rgba;
    e.layer = layer;
    e.path = path;
    stream_.append(e);
    return id;
  }

  StackStatus undo_last() override {
    if (set_ == nullptr) return StackStatus::Fail(StackCode::OutOfGrid, "recorder 未 attach");
    std::vector<std::uint32_t> ids;
    set_->strokes_in_order(&ids);
    if (ids.empty()) return StackStatus::Fail(StackCode::UnknownStroke, "没有可撤销的笔迹");
    return undo(ids.back());
  }

  StackStatus undo(std::uint32_t stroke_id) override {
    if (set_ == nullptr) return StackStatus::Fail(StackCode::OutOfGrid, "recorder 未 attach");
    std::vector<std::size_t> idx;
    set_->indices_of_stroke(stroke_id, &idx);
    if (idx.empty()) {
      return StackStatus::Fail(StackCode::UnknownStroke,
                               "撤销失败: 不存在 stroke_id " + std::to_string(stroke_id));
    }
    if (auto s = set_->remove_stroke(stroke_id); !s.ok()) return s;
    EventRecord e;
    e.kind = EventKind::Undo;
    e.stroke_id = stroke_id;
    stream_.append(e);
    return StackStatus::Ok_();
  }

  StackStatus set_color(const Cell& cell, std::uint32_t rgba) override {
    if (set_ == nullptr) return StackStatus::Fail(StackCode::OutOfGrid, "recorder 未 attach");
    const Block* b = set_->find(cell);
    if (b == nullptr) {
      return StackStatus::Fail(StackCode::OutOfGrid,
                               "改色失败: 格 (" + std::to_string(cell.x) + "," +
                                   std::to_string(cell.y) + "," + std::to_string(cell.z) +
                                   ") 没有方块");
    }
    const std::size_t index = static_cast<std::size_t>(b - set_->data());
    if (auto s = set_->set_color(index, rgba); !s.ok()) return s;
    EventRecord e;
    e.kind = EventKind::SetColor;
    e.cell = cell;
    e.color = rgba;
    stream_.append(e);
    return StackStatus::Ok_();
  }

  StackStatus lock_layer(std::int32_t layer) override {
    if (brush_ == nullptr) return StackStatus::Fail(StackCode::OutOfGrid, "recorder 未 attach");
    if (auto s = brush_->layer_lock().lock(layer); !s.ok()) return s;
    locked_ = layer;
    EventRecord e;
    e.kind = EventKind::LockLayer;
    e.layer = layer;
    stream_.append(e);
    return StackStatus::Ok_();
  }

  StackStatus unlock_layer() override {
    if (brush_ == nullptr) return StackStatus::Fail(StackCode::OutOfGrid, "recorder 未 attach");
    brush_->layer_lock().unlock();
    locked_ = -1;
    EventRecord e;
    e.kind = EventKind::UnlockLayer;
    stream_.append(e);
    return StackStatus::Ok_();
  }

  std::int32_t locked_layer() const override { return locked_; }

 private:
  IBlockSet* set_ = nullptr;
  GridSpec grid_{};
  std::unique_ptr<IBlockBrush> brush_;
  CommandStream stream_;
  std::int32_t locked_ = -1;
};

}  // namespace

std::unique_ptr<ICommandStream> create_command_stream() { return std::make_unique<CommandStream>(); }

std::unique_ptr<IRecorder> create_recorder() { return std::make_unique<Recorder>(); }

StackResult<ReplayStats> replay(const ICommandStream& stream, IBlockSet& set,
                                  const GridSpec& grid) {
  ReplayStats st;
  auto brush = create_block_brush(BrushConfig{grid, 1});
  if (!brush) return err(StackCode::OutOfGrid, "replay: 笔刷创建失败");
  std::int32_t locked = -1;

  for (std::size_t i = 0; i < stream.size(); ++i) {
    const EventRecord& e = stream.at(i);
    switch (e.kind) {
      case EventKind::Stroke: {
        const std::int32_t eff_layer = (e.layer >= 0) ? e.layer : (grid.layers - 1);
        if (e.layer >= 0) {
          if (auto s = brush->layer_lock().lock(e.layer); !s.ok()) return s.error();
        } else {
          brush->layer_lock().unlock();
        }
        const float plane_y =
            grid.origin[1] + (static_cast<float>(eff_layer) + 0.5f) * grid.cell_size;
        std::vector<Ray> rays;
        rays.reserve(e.path.size());
        for (const PathPoint& p : e.path) {
          rays.push_back(Ray{Vec3{static_cast<float>(p.px) * grid.cell_size, plane_y + 8.0f,
                                  static_cast<float>(p.py) * grid.cell_size},
                             Vec3{0.0f, -1.0f, 0.0f}});
        }
        if (rays.empty()) return err(StackCode::EmptyStroke, "replay: 笔迹路径为空");
        auto plan_r = brush->stroke(rays.data(), rays.size());
        if (!plan_r.ok()) return plan_r.error();
        const StrokePlan plan = plan_r.value();

        std::vector<Cell> fresh;
        fresh.reserve(plan.cells.size());
        for (const Cell& c : plan.cells) {
          if (set.find(c) == nullptr) fresh.push_back(c);
        }
        ++st.strokes;
        if (fresh.empty()) break;  // 记录态当时就是"整笔被占"，回放同样什么都不做
        // **M10：原值透传，绝不 next_stroke_id() 重新编号。**
        if (auto s = add_stroke(&set, fresh, e.color, e.stroke_id); !s.ok()) return s.error();
        break;
      }
      case EventKind::Undo:
        {
          std::vector<std::size_t> idx;
          set.indices_of_stroke(e.stroke_id, &idx);
          if (!idx.empty()) {
            if (auto s = set.remove_stroke(e.stroke_id); !s.ok()) return s.error();
          }
        }
        ++st.undos;
        break;
      case EventKind::SetColor: {
        const Block* b = set.find(e.cell);
        if (b == nullptr) return err(StackCode::OutOfGrid, "replay: 改色的格没有方块");
        const std::size_t index = static_cast<std::size_t>(b - set.data());
        if (auto s = set.set_color(index, e.color); !s.ok()) return s.error();
        ++st.color_sets;
        break;
      }
      case EventKind::LockLayer:
        if (auto s = brush->layer_lock().lock(e.layer); !s.ok()) return s.error();
        locked = e.layer;
        ++st.locks;
        break;
      case EventKind::UnlockLayer:
        brush->layer_lock().unlock();
        locked = -1;
        ++st.locks;
        break;
    }
  }
  (void)locked;

  st.blocks = static_cast<std::uint32_t>(set.size());
  st.result_hash = set.hash();
  return st;
}

StackResult<BuildResult> build(const std::vector<StrokeScript>& script, const GridSpec& grid) {
  auto set_r = create_block_set(grid);
  if (!set_r.ok()) return set_r.error();
  auto set = std::move(set_r).take();

  auto rec = create_recorder();
  if (auto s = rec->attach(set.get(), grid); !s.ok()) return s.error();

  for (const StrokeScript& s : script) {
    auto id = rec->stroke(s.path, s.rgba, s.layer);
    if (!id.ok()) return id.error();
  }

  BuildResult r;
  r.stream_hash = rec->stream().hash();
  r.set_hash = set->hash();
  r.blocks = static_cast<std::uint32_t>(set->size());
  r.stroke_count = rec->stream().size();
  return r;
}

StackResult<CommandCheck> check_three_states(const std::vector<StrokeScript>& script,
                                             const GridSpec& grid) {
  CommandCheck chk;

  // ---- 录制态 ----
  auto set_r = create_block_set(grid);
  if (!set_r.ok()) return set_r.error();
  auto rec_set = std::move(set_r).take();
  auto rec = create_recorder();
  if (auto s = rec->attach(rec_set.get(), grid); !s.ok()) return s.error();
  for (const StrokeScript& s : script) {
    auto id = rec->stroke(s.path, s.rgba, s.layer);
    if (!id.ok()) return id.error();
  }
  chk.recorded_set_hash = rec_set->hash();
  chk.recorded_stream_hash = rec->stream().hash();
  const std::uint32_t recorded_blocks = static_cast<std::uint32_t>(rec_set->size());

  // ---- 回放态 ----
  auto rep_r = create_block_set(grid);
  if (!rep_r.ok()) return rep_r.error();
  auto rep_set = std::move(rep_r).take();
  // 回放走**序列化 → 解析 → 重放**这条完整路径，而不是直接拿内存里的流对象。
  // 这是故意的：只有过一遍文本，才能证明"录制态可落盘"与"回放读的是同一份语义"。
  const std::string text = rec->stream().serialize();
  auto parsed = create_command_stream();
  std::string detail;
  if (!parsed->parse(text, &detail)) {
    return err(StackCode::OutOfGrid, "回放: 命令流解析失败: " + detail);
  }
  auto rp = replay(*parsed, *rep_set, grid);
  if (!rp.ok()) return rp.error();
  chk.replayed_set_hash = rep_set->hash();
  chk.set_hash_equal = (chk.recorded_set_hash == chk.replayed_set_hash);

  // 流的往返一致性：解析回来的流必须与录制流同哈希
  // （抓"序列化丢字段"，IBlockSet 的 hash 抓不到这类问题）。
  chk.stream_hash_equal = (parsed->hash() == chk.recorded_stream_hash);

  // ---- 复现态 ----
  auto again = build(script, grid);
  if (!again.ok()) return again.error();
  chk.reproduced_set_hash = again.value().set_hash;
  chk.reproduced_stream_hash = again.value().stream_hash;
  chk.reproduce_equal = (again.value().set_hash == chk.recorded_set_hash) &&
                        (again.value().stream_hash == chk.recorded_stream_hash);

  chk.blocks_equal = (recorded_blocks == static_cast<std::uint32_t>(rep_set->size())) &&
                     (recorded_blocks == again.value().blocks);

  return chk;
}

}  // namespace magicblocks::features::stack
