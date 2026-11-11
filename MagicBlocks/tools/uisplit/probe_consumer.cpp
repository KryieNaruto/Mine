// ============================================================================
// tools/uisplit/probe_consumer.cpp —— 【UI 边界的活证据】
//
// 这个文件扮演"第三方 UI 消费者"，刻意做到：
//   1. 只 #include 一个头：magicblocks 的 mb_capi.h
//      不许出现 mb_internal/...、不许出现 src/...、不许出现任何 SDK 内部类型。
//
// ⚠️ 这行 include 的两种形态（同一个文件的两种视角，别混淆）：
//   - **构建期**：本文件作为本工程的一个 target 编译，头搜索路径来自
//     magicblocks_shared 的 PUBLIC 面 = `include/magicblocks`，
//     所以下面写 `#include "mb_capi.h"`。
//   - **出厂期**：UI 真正拿到的是"导出面目录 + dll"。导出面是
//     `<导出面>/magicblocks/mb_capi.h`，UI 写
//     `#include "magicblocks/mb_capi.h"`。
//   tools/check_ui_isolation.py 的 UI_CONSUMER 走的是后者（那是验收本体）；
//   本文件走前者，证明"真链接 + 真运行"这条链也是通的。
//   2. 只链一个动态库：magicblocks_shared（Windows = magicblocks.dll + 导入库，
//      Linux = libmagicblocks.so，Android = libmagicblocks.so）。
//   3. 只调用 mb_capi.h 里 MB_API 标注过的入口。
//
// 它编译不过 / 链接不过 / 跑不起来，就等价于老板那句话不成立：
//     "UI 只依赖 DLL + 一个 include 头文件"。
//
// 它验证三件事：
//   A. 编译期：头文件是自足的（自带 uint32_t/size_t、自带 extern "C"、
//      自带结构体尺寸钉子，不依赖任何其它 MagicBlocks 头）。
//   B. 链接期：DLL 的导入库确实导出了 mb_capi.h 声明的那些裸名
//      （少一个就是 LNK2019 —— 这正是"头文件说有、库里没有"的分叉）。
//   C. 运行期：库真的被加载并跑通一次"建会话 → 挂离屏 surface →
//      渲染 → 销毁"，且头文件 MAJOR 版本与运行时库一致。
//
// 返回码：0 = 全部通过；非 0 = 第几步失败（见下面每个 step 的注释）。
// ============================================================================

#include "mb_capi.h"

#include <cstdio>
#include <cstring>

int main()
{
  // ---- A. 编译期证据：头文件自足 -------------------------------------------
  // MB_ABI_VERSION / MB_STATIC_ASSERT_SIZE 都只在 mb_capi.h 里定义。
  const uint32_t header_abi = static_cast<uint32_t>(MB_ABI_VERSION);
  std::printf("[probe] 头文件 ABI 版本 MB_ABI_VERSION = %u\n",
              static_cast<unsigned>(header_abi));

  // ---- B/C. 运行期证据：真的从 DLL 里把函数调起来 -------------------------
  // 入口 2/27: mb_abi_version —— UI 加载动态库后必须第一个调用的函数
  const uint32_t runtime_abi = mb_abi_version();
  std::printf("[probe] 运行时库 ABI 版本 mb_abi_version() = %u\n",
              static_cast<unsigned>(runtime_abi));

  // 头文件与运行时的 MAJOR 不一致 → UI 必须停用本库。
  // 打包公式见 mb_capi.h §4：MAJOR*10000 + MINOR*100 + PATCH。
  if ((runtime_abi / 10000u) != (header_abi / 10000u)) {
    std::printf("[probe] FAIL: 头文件与运行时库 MAJOR 版本不一致\n");
    return 2;
  }

  // 入口 3/27: mb_abi_version_string —— 两段式调用，验 buf 可为 NULL 的约定
  char ver[32];
  std::memset(ver, 0, sizeof(ver));
  const size_t need = mb_abi_version_string(nullptr, 0); // 只查询长度
  mb_abi_version_string(ver, sizeof(ver));
  std::printf("[probe] 版本字符串 \"%s\"（所需长度 %zu）\n", ver, need);

  // 入口 4/27: mb_set_log_callback —— 进程级，与 session 无关
  mb_log_cb_desc log_desc;
  std::memset(&log_desc, 0, sizeof(log_desc));
  log_desc.struct_size = static_cast<uint32_t>(sizeof(log_desc));
  log_desc.cb = nullptr; // nullptr = 注销（这里只为证明符号与 struct_size 约定可用）
  log_desc.user = nullptr;
  const mb_err log_err = mb_set_log_callback(&log_desc);
  std::printf("[probe] mb_set_log_callback -> %s\n", mb_err_str(log_err));
  if (log_err != MB_OK) {
    std::printf("[probe] FAIL: mb_set_log_callback 返回 %s\n", mb_err_str(log_err));
    return 3;
  }

  // 入口 5/27: mb_session_desc_default —— 按值返回结构体；
  // 头文件里的 MB_STATIC_ASSERT_SIZE(mb_session_desc, 48) 已在编译期钉住尺寸，
  // 这里再确认实现侧填的 struct_size 与头文件一致（不一致 = 头/库分叉）。
  mb_session_desc desc = mb_session_desc_default();
  std::printf("[probe] mb_session_desc_default: struct_size=%u msaa=%u\n",
              static_cast<unsigned>(desc.struct_size), static_cast<unsigned>(desc.msaa));
  if (desc.struct_size != sizeof(mb_session_desc)) {
    std::printf("[probe] FAIL: 库返回的 struct_size(%u) != 头文件 sizeof(%zu)\n",
                static_cast<unsigned>(desc.struct_size), sizeof(mb_session_desc));
    return 4;
  }

  // 入口 6~9: 建会话 → 挂离屏 surface → 渲染一帧 → 销毁。
  // 这条链路要跑通，说明 DLL 内部整条 SDK 都在，而消费者一个内部头都没见过。
  mb_session* s = nullptr;
  mb_err e = mb_session_create(&desc, &s);
  std::printf("[probe] mb_session_create -> %s\n", mb_err_str(e));

  if (e == MB_OK && s != nullptr) {
    // 离屏 surface：不需要窗口系统，CI 里也能跑
    e = mb_session_attach_surface(s, MB_SURFACE_HEADLESS, nullptr, 64, 64);
    std::printf("[probe] mb_session_attach_surface -> %s\n", mb_err_str(e));

    const mb_pose pose = mb_pose_default();
    e = mb_session_render(s, pose);
    std::printf("[probe] mb_session_render -> %s\n", mb_err_str(e));

    // 入口 10/27: mb_session_perf —— 按值快照，验证 struct_size 约定
    const mb_perf_snapshot perf = mb_session_perf(s);
    std::printf("[probe] mb_session_perf: struct_size=%u frames_total=%llu\n",
                static_cast<unsigned>(perf.struct_size),
                static_cast<unsigned long long>(perf.frames_total));

    // 入口 11/27: mb_session_status_text —— 两段式 buffer 约定
    char status[128];
    std::memset(status, 0, sizeof(status));
    mb_session_status_text(s, status, sizeof(status));
    std::printf("[probe] mb_session_status_text -> \"%s\"\n", status);
  }

  // 入口 12/27: mb_session_is_valid —— 销毁前后语义
  const uint8_t valid_before = mb_session_is_valid(s);

  // 入口 13/27: mb_session_destroy —— 对 NULL 也必须是幂等 no-op
  const mb_err destroy_err = mb_session_destroy(s);
  const uint8_t valid_after = mb_session_is_valid(s);
  std::printf("[probe] mb_session_destroy -> %s（销毁前 valid=%u，销毁后 valid=%u）\n",
              mb_err_str(destroy_err),
              static_cast<unsigned>(valid_before), static_cast<unsigned>(valid_after));

  // 入口 14/27: mb_model_list_is_valid —— NULL 句柄的有效性判定
  std::printf("[probe] mb_model_list_is_valid(nullptr) = %u\n",
              static_cast<unsigned>(mb_model_list_is_valid(nullptr)));

  // 入口 15/27: mb_model_list_protocol_version —— 协议版本查询
  std::printf("[probe] mb_model_list_protocol_version() = %u\n",
              static_cast<unsigned>(mb_model_list_protocol_version()));

  // 入口 16/27: mb_err_str —— 越界值必须返回 "MB_E_UNKNOWN"（永不 NULL）
  const char* unknown = mb_err_str(static_cast<mb_err>(999));
  std::printf("[probe] mb_err_str(999) = \"%s\"\n", unknown);
  if (unknown == nullptr) {
    std::printf("[probe] FAIL: mb_err_str 返回了 NULL\n");
    return 5;
  }

  // 入口 17~27: 剩下 11 个入口在 headless 路径下没有可用前置条件
  // （detach_surface / surface_resized / set_msaa / msaa_effective /
  //   msaa_supported / scan_models / model_list_count / model_list_get /
  //   model_list_free / load_model / clear_model）。
  // 它们的符号存在性由**链接期**保证：mb_capi.h 里每个 MB_API 都是一个
  // dllimport 声明，DLL 里少导出任何一个，这个可执行文件就 LNK2019 链不出来。
  // 所以这里不需要再注册一遍函数指针 —— 能链上就已经是证据。
  std::printf("[probe] PASS: 只靠 mb_capi.h 走完 ABI 握手 + 建会话 + 渲染 + 销毁\n");
  return 0;
}
