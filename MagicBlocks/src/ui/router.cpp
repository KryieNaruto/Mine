// MagicBlocks UI 消费层 —— 功能路由与特性宿主实现(spec §4)
//
// 栈深度刻意限制为 1 层玩法(HomeScreen 恒为根,不入栈):
// spec §9 明确「多窗口/多 surface 不做」,demo 期也没有"玩法里再进玩法"的需求,
// 限制住栈深度可以让 pop() 的语义无歧义,也让 G7(切后台)的状态保存只有一个出口。
#include "mb_internal/magicblocks/ui/router.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <utility>

namespace magicblocks::ui {

const char* to_string(RouteAction action) {
  switch (action) {
    case RouteAction::None: return "None";
    case RouteAction::Pushed: return "Pushed";
    case RouteAction::Popped: return "Popped";
    case RouteAction::Failed: return "Failed";
  }
  return "Unknown";
}

// ---------------------------------------------------------------- MemoryStateStore

struct MemoryStateStore::Impl {
  std::unordered_map<std::string, StateBlob> blobs;
};

MemoryStateStore::MemoryStateStore() : impl_(std::make_unique<Impl>()) {}
MemoryStateStore::~MemoryStateStore() = default;

void MemoryStateStore::put(const FeatureId& id, StateBlob blob) {
  impl_->blobs[id.value] = std::move(blob);
}

const StateBlob* MemoryStateStore::get(const FeatureId& id) const {
  auto it = impl_->blobs.find(id.value);
  return it == impl_->blobs.end() ? nullptr : &it->second;
}

void MemoryStateStore::remove(const FeatureId& id) { impl_->blobs.erase(id.value); }

std::size_t MemoryStateStore::size() const { return impl_->blobs.size(); }

// 序列化:纯文本、可 diff、无第三方依赖(与 SDK 命令流"可选 JSON 文本"同一思路)。
// 格式(每行一条,空行忽略):
//   MBSTATE 1
//   NODE <id> <parent> <visible 0|1> <t0> <t1> <t2> <r0> <r1> <r2> <r3> <s0> <s1> <s2>
//   TOOL <u64>
//   CAMERA <9 个 float>
std::string MemoryStateStore::serialize(const StateBlob& blob) const {
  std::string out = "MBSTATE 1\n";
  char buf[512];
  for (const auto& n : blob.scene.nodes) {
    std::snprintf(buf, sizeof(buf),
                  "NODE %llu %llu %d %g %g %g %g %g %g %g %g %g %g\n",
                  static_cast<unsigned long long>(n.id),
                  static_cast<unsigned long long>(n.parent), n.visible ? 1 : 0,
                  static_cast<double>(n.transform.translation[0]),
                  static_cast<double>(n.transform.translation[1]),
                  static_cast<double>(n.transform.translation[2]),
                  static_cast<double>(n.transform.rotation[0]),
                  static_cast<double>(n.transform.rotation[1]),
                  static_cast<double>(n.transform.rotation[2]),
                  static_cast<double>(n.transform.rotation[3]),
                  static_cast<double>(n.transform.scale[0]),
                  static_cast<double>(n.transform.scale[1]),
                  static_cast<double>(n.transform.scale[2]));
    out += buf;
  }
  std::snprintf(buf, sizeof(buf), "TOOL %llu\n",
                static_cast<unsigned long long>(blob.tool_params));
  out += buf;
  if (blob.has_camera) {
    out += "CAMERA";
    for (int i = 0; i < 9; ++i) {
      std::snprintf(buf, sizeof(buf), " %g", static_cast<double>(blob.camera[i]));
      out += buf;
    }
    out += "\n";
  }
  return out;
}

bool MemoryStateStore::deserialize(const std::string& text, StateBlob* out) const {
  if (!out) return false;
  StateBlob blob;
  bool header = false;
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;
    if (line.empty()) continue;
    if (!header) {
      if (line.rfind("MBSTATE ", 0) != 0) return false;  // 版本头不匹配即明确失败
      header = true;
      continue;
    }
    char tag[32] = {0};
    if (line.rfind("NODE ", 0) == 0) {
      NodeSnapshot n{};
      unsigned long long id = 0, parent = 0;
      int vis = 1;
      float t[3], r[4], s[3];
      if (std::sscanf(line.c_str(), "NODE %llu %llu %d %f %f %f %f %f %f %f %f %f %f", &id,
                      &parent, &vis, &t[0], &t[1], &t[2], &r[0], &r[1], &r[2], &r[3], &s[0],
                      &s[1], &s[2]) != 13) {
        return false;
      }
      n.id = id;
      n.parent = parent;
      n.visible = vis != 0;
      for (int i = 0; i < 3; ++i) {
        n.transform.translation[i] = t[i];
        n.transform.scale[i] = s[i];
      }
      for (int i = 0; i < 4; ++i) n.transform.rotation[i] = r[i];
      blob.scene.nodes.push_back(n);
    } else if (line.rfind("TOOL ", 0) == 0) {
      unsigned long long v = 0;
      if (std::sscanf(line.c_str(), "TOOL %llu", &v) != 1) return false;
      blob.tool_params = v;
    } else if (line.rfind("CAMERA", 0) == 0) {
      float c[9];
      if (std::sscanf(line.c_str(), "CAMERA %f %f %f %f %f %f %f %f %f", &c[0], &c[1], &c[2],
                      &c[3], &c[4], &c[5], &c[6], &c[7], &c[8]) != 9) {
        return false;
      }
      for (int i = 0; i < 9; ++i) blob.camera[i] = c[i];
      blob.has_camera = true;
    } else {
      (void)tag;
      return false;  // 未知行:明确失败,不"尽力解析"
    }
  }
  if (!header) return false;
  *out = std::move(blob);
  return true;
}

// ---------------------------------------------------------------- Router

struct Router::Impl {
  std::vector<FeatureRegistration> regs;
  IStateStore* store = nullptr;
  FeatureContext ctx{};

  FeatureId current_id{};
  std::unique_ptr<IFeatureHost> host;  // 非空 <=> 在某个玩法内

  FeatureRegistration* find(const FeatureId& id) {
    for (auto& r : regs) {
      if (r.id == id) return &r;
    }
    return nullptr;
  }

  // 退出当前玩法:保存状态 → on_exit → 卸载。所有退出路径共用这一个函数。
  void teardown_current() {
    if (!host) return;
    if (store && ctx.scene) {
      StateBlob blob;
      blob.scene = ctx.scene->snapshot();
      store->put(current_id, std::move(blob));
    }
    host->on_exit();
    host.reset();
    current_id = FeatureId{};
  }
};

Router::Router() : impl_(std::make_unique<Impl>()) {}
Router::~Router() = default;

void Router::register_feature(FeatureRegistration reg) {
  if (reg.id.empty() || !reg.factory) return;
  if (FeatureRegistration* existing = impl_->find(reg.id)) {
    *existing = std::move(reg);
    return;
  }
  impl_->regs.push_back(std::move(reg));
}

std::size_t Router::feature_count() const { return impl_->regs.size(); }

void Router::set_state_store(IStateStore* store) { impl_->store = store; }

void Router::set_context(FeatureContext ctx) { impl_->ctx = ctx; }

RouteAction Router::push(const FeatureId& id) {
  if (id.empty()) return RouteAction::Failed;
  FeatureRegistration* reg = impl_->find(id);
  if (!reg) return RouteAction::Failed;

  // 已在某个玩法内 → 先正常退出(保存 + on_exit),再进入新的。
  // 这样"玩法间切换"走的是同一套生命周期,不会出现两份玩法同时活着。
  if (impl_->host) {
    impl_->teardown_current();
  }

  std::unique_ptr<IFeatureHost> next = reg->factory();
  if (!next) return RouteAction::Failed;

  // 重进恢复:内存优先(spec §4「存内存为主」),不触碰磁盘。
  if (impl_->store && impl_->ctx.scene) {
    if (const StateBlob* blob = impl_->store->get(id)) {
      if (!blob->scene.nodes.empty()) impl_->ctx.scene->restore(blob->scene);
    }
  }

  impl_->current_id = id;
  impl_->host = std::move(next);
  impl_->host->on_enter(impl_->ctx);
  return RouteAction::Pushed;
}

RouteAction Router::pop() {
  if (!impl_->host) return RouteAction::Failed;
  impl_->teardown_current();
  return RouteAction::Popped;
}

const FeatureId& Router::current() const { return impl_->current_id; }

void Router::update(f32 dt_seconds) {
  if (impl_->host) impl_->host->on_update(dt_seconds);
}

bool Router::on_input(const device::InputEvent& ev) {
  if (!impl_->host) return false;  // HomeScreen:交给相机/UI
  return impl_->host->on_input(ev);
}

IFeatureHost* Router::current_host() const { return impl_->host.get(); }

RouteCallback Router::callback() {
  // 持有 Router*:Router 生命周期由 UI 层保证长于 HUD 回调(壳层持有 UiApp)。
  return [this](const FeatureId& id) -> RouteAction {
    if (id.empty()) return pop();
    return push(id);
  };
}

}  // namespace magicblocks::ui
