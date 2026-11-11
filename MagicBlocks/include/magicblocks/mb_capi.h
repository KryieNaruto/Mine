/*
 * mb_capi.h —— MagicBlocks UI ⇄ SDK 的**唯一契约**（stable C ABI）
 * ============================================================================
 *
 * 【这是什么】
 *   这是 UI 层（Android Compose 壳 + Kotlin、未来任何其它壳）**唯一**允许依赖的
 *   SDK 头文件。UI 只依赖两个东西：
 *       1) 本头文件；
 *       2) 一个动态库产物（Windows: magicblocks.dll / Android: libmagicblocks.so）。
 *   SDK 内部（device/、resources/、primitives/、backend/、features/ … ）怎么重构、
 *   怎么加虚函数、怎么换后端，都不会改变本文件的任何一行，因此**不影响 UI 的编译**。
 *
 * 【修改规则 —— 违反即返工】
 *   1. 本头文件是 UI 的 ABI。**任何**改动（加/删/改函数签名、改枚举值、改结构体字段
 *      或顺序、改错误码含义）都**必须 bump `mb_abi_version()` 的返回值**
 *      （见下方 §0 的版本三元组与 bump 规则）。不 bump 的改动一律打回。
 *   2. SDK 内部头文件（`magicblocks/device/`、`magicblocks/resources/`、
 *      `magicblocks/primitives/`、`magicblocks/backend/`、`magicblocks/scene/`、
 *      `magicblocks/features/`、`magicblocks/ui/` 等）**一律不得**出现在本文件里 ——
 *      包括 include、前置声明、typedef 别名、注释里"仅供参考"的签名摘抄。
 *      本文件只 include 标准 C 头 `<stdint.h>` 与 `<stddef.h>`，不接受任何例外。
 *   3. 本文件是**纯 C**（C99 起）：不出现 `std::`、模板、引用、`class`、异常、RTTI。
 *      用 C++ 编译时由 `extern "C"` 包裹，保证符号不被 mangle。
 *   4. 所有结构体**按值传/按值返回**且大小固定（字段只用定长标量与定长数组）。
 *      唯一的指针成员是日志回调的描述符（见 §3），且它只用于**注册**，不跨帧传递。
 *   5. 字符串一律两种形态之一：
 *        - 入参：`const char*`（UTF-8、NUL 结尾，调用方拥有）；
 *        - 出参：`char* buf + size_t buf_size`（调用方给缓冲区 + 长度，填充式），
 *          函数返回**实际所需长度**（含结尾 NUL），便于调用方按需重试。
 *      **绝不**返回需要 UI 释放的 SDK 内存（没有 `mb_free`，也没有隐式所有权）。
 *   6. 句柄一律不透明：`typedef struct mb_session mb_session;`
 *      调用方**不得**解引用、不得做指针算术、不得跨库 `free`。
 *
 * 【版本策略】
 *   MB_ABI_VERSION_MAJOR  — 破坏性变更（删/改函数签名或结构体字段、改语义）
 *   MB_ABI_VERSION_MINOR  — 只增不改（新增函数、在结构体**尾部**追加字段）
 *   MB_ABI_VERSION_PATCH  — 纯实现修复，ABI 完全不变
 *   `mb_abi_version()` 返回打包后的 `MAJOR*10000 + MINOR*100 + PATCH`。
 *   调用方应在 `loadLibrary` 之后**第一件事**调用它，与本头文件的
 *   `MB_ABI_VERSION_*` 比对：MAJOR 不一致就**不要继续调用任何其它入口**。
 *
 * 【错误模型】
 *   所有可能失败的入口返回 `mb_err`。`MB_OK(0)` 表示成功。
 *   `mb_err_str()` 给出**稳定英文标识**（永不返回 NULL）—— SDK 不产出面向用户的
 *   文案，本地化是 UI 的事。需要人类可读细节时用 `mb_session_status_text()`。
 *
 * 【线程模型】
 *   - `mb_session_*` 除 `mb_session_render` / `mb_session_perf` / `mb_session_status_text`
 *     之外都设计为"UI 线程调用"；
 *   - `mb_session_render` 可以在 UI 线程或**单一**渲染线程上调用（不要两处并发）；
 *   - `mb_session_perf` / `mb_session_status_text` 可从任意线程调用（内部是原子快照）；
 *   - 同一 `mb_session` 上的 `detach_surface` 与 `render` 由库内部串行化，
 *     调用方**不需要**自己等渲染线程退出（这一点与旧的 JNI 契约不同，是改进）。
 */

#ifndef MAGICBLOCKS_MB_CAPI_H
#define MAGICBLOCKS_MB_CAPI_H

#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * 0. 平台宏冲突：`MB_OK`
 *
 * Windows 的 `<windows.h>`（经 wingdi.h）里有一行 `#define MB_OK 0L` —— MessageBox 的
 * 返回值。谁先 include 谁说了算：若调用方先 include 了 windows.h，下面 §2 的
 * `MB_OK = 0` 会被替换成 `0L = 0`，**整个头文件编译失败**（C2059 连环）。
 *
 * 这不是"记得调整包含顺序"就能糊过去的事：Windows 上的 UI 侧几乎一定要 include
 * windows.h，而 ABI 头必须能被任何包含顺序接受。同时 `MB_OK` 这个错误码名已经写进
 * ABI（t2 的契约、Kotlin 侧的 `err=MB_OK` 文本协议都在用它），不能改名。
 *
 * 处理办法：本文件在自己的作用域内把平台的 `MB_OK` **临时压掉**，走到文件末尾再按
 * Windows 的原文本恢复（`0x00000000L`，见文件末尾的说明）。
 *   - 对 SDK 自己的代码：`MB_OK` 是枚举常量，与平台宏语义一致（都是 0），无副作用；
 *   - 对调用方：include 完之后 `MB_OK` 仍是同一个平台宏定义，不会因为"包含了 SDK 头"
 *     而让 `MessageBox(...) == MB_OK` 之类代码变形。
 * ========================================================================== */
#if defined(MB_OK)
#  undef MB_OK
#  define MB_CAPI_RESTORED_MB_OK 1
#endif

/* 全部入口用 C 链接，C++ 编译时不得 mangle 符号。
 * 没有这一段，C++ 侧会得到 ?mb_session_create@@YA... 这种修饰名，和 DLL 里的
 * 未修饰导出名对不上 —— 链接期直接 LNK2019。 */
#if defined(__cplusplus)
extern "C" {
#endif

/* ============================================================================
 * 0. ABI 版本
 * ========================================================================== */

/** 破坏性变更时 +1。 */
#define MB_ABI_VERSION_MAJOR 1
/** 只增不改时 +1。 */
#define MB_ABI_VERSION_MINOR 0
/** 纯修复。 */
#define MB_ABI_VERSION_PATCH 0

/** 打包后的 ABI 版本号；`mb_abi_version()` 运行时返回同一个打包公式的值。 */
#define MB_ABI_VERSION ((uint32_t)(MB_ABI_VERSION_MAJOR * 10000u + \
                                   MB_ABI_VERSION_MINOR * 100u + \
                                   MB_ABI_VERSION_PATCH))

/** 编译期断言：本头文件的版本不低于 (maj, min)。示例：
 *  `#if !MB_ABI_AT_LEAST(1, 0)  #error "SDK 头文件太旧"  #endif` */
#define MB_ABI_AT_LEAST(maj, min) \
  (MB_ABI_VERSION_MAJOR > (maj) || \
   (MB_ABI_VERSION_MAJOR == (maj) && MB_ABI_VERSION_MINOR >= (min)))

/* ============================================================================
 * 1. 导出宏 / 调用约定 / 编译期尺寸钉子
 *
 * 必须放在所有函数声明**之前**：函数返回类型是 typedef 名，而 MB_API 是修饰符，
 * 两者混用（宏名与 typedef 名同形）会在 C 模式下直接编译失败。
 * ========================================================================== */

#if defined(_WIN32) || defined(__CYGWIN__)
#  define MB_API_EXPORT __declspec(dllexport)
#  define MB_API_IMPORT __declspec(dllimport)
#  define MB_CALL __cdecl
#else
#  define MB_API_EXPORT __attribute__((visibility("default")))
#  define MB_API_IMPORT __attribute__((visibility("default")))
#  define MB_CALL
#endif

/* 库自身实现（由构建系统定义 MB_CAPI_BUILDING_LIBRARY=1）导出，消费者导入。 */
#if defined(MB_CAPI_BUILDING_LIBRARY)
#  define MB_API MB_API_EXPORT
#else
#  define MB_API MB_API_IMPORT
#endif

/* 结构体尺寸钉子：换编译器/换架构时任何 padding 差异都在**编译期**炸。
 * C 分支不用 `##__LINE__`（标准 C 里属实现定义，MSVC 会直接报错），而是用
 * "同名 typedef 两次"：条件成立时两处一致（合法），不成立时一处是 char、
 * 一处是 char[-1]（必然报错）。 */
#if defined(__cplusplus)
#  define MB_STATIC_ASSERT_SIZE(type, expected) \
     static_assert(sizeof(type) == (expected), #type " 的 ABI 尺寸不符")
#else
#  define MB_STATIC_ASSERT_SIZE(type, expected) \
     typedef char mb_size_check_##type[(sizeof(type) == (expected)) ? 1 : -1]; \
     typedef char mb_size_check_##type[(sizeof(type) == (expected)) ? 1 : -1]
#endif

/* ============================================================================
 * 2. 错误码
 * ========================================================================== */

typedef enum mb_err {
  MB_OK = 0,                    /**< 成功 */
  MB_E_INVALID_ARG = 1,         /**< 参数非法（NULL / 越界 / 尺寸为 0） */
  MB_E_NOT_INITIALIZED = 2,     /**< 会话尚未初始化完成（设备/资源没建起来） */
  MB_E_NO_SURFACE = 3,          /**< 没有挂载 surface，或已被卸载 */
  MB_E_SURFACE_LOST = 4,        /**< surface 已失效（如 Android surfaceDestroyed），需重新挂载 */
  MB_E_OUT_OF_DATE = 5,         /**< 交换链与 surface 尺寸不匹配；库已内部重建，本帧丢弃即可 */
  MB_E_DEVICE_LOST = 6,         /**< GPU 设备丢失（不可恢复，需销毁并重建会话） */
  MB_E_UNSUPPORTED = 7,         /**< 当前平台/后端不支持该能力 */
  MB_E_OUT_OF_MEMORY = 8,       /**< 显存/内存预算不足 */
  MB_E_IO = 9,                  /**< 文件读写失败（模型/资产） */
  MB_E_NOT_FOUND = 10,          /**< 目录或文件不存在（与"空目录"严格区分） */
  MB_E_BUSY = 11,               /**< 资源正被渲染占用，调用方应稍后重试 */
  MB_E_VERSION_MISMATCH = 12,   /**< 头文件 ABI 与运行时库 ABI 不兼容 */
  MB_E_INTERNAL = 13,           /**< 库内部缺陷（status 文本里有具体位置） */
  MB_E__COUNT = 14              /**< 仅供边界检查，不作为返回值 */
} mb_err;

/** 错误码 → 稳定英文标识（`"MB_E_NO_SURFACE"` 等）。**永不返回 NULL**，
 *  对越界值返回 `"MB_E_UNKNOWN"`。 */
MB_API const char* MB_CALL mb_err_str(mb_err e);

/* ============================================================================
 * 3. 不透明句柄
 * ========================================================================== */

/** 会话句柄（一次引擎生命周期 = 一个 session）。完全不透明。 */
typedef struct mb_session mb_session;

/** 模型列表句柄（`mb_session_scan_models` 产出，`mb_model_list_free` 释放）。 */
typedef struct mb_model_list mb_model_list;

/** 句柄有效性（非 NULL 且内部 magic 正确）。用来替代 C++ 的异常/判空惯例：
 *  对**任何**入口传入无效句柄都只会得到 `MB_E_INVALID_ARG`，不会崩溃。 */
MB_API uint8_t MB_CALL mb_session_is_valid(const mb_session* s);
MB_API uint8_t MB_CALL mb_model_list_is_valid(const mb_model_list* l);

/* ============================================================================
 * 4. 版本查询
 * ========================================================================== */

/** 返回运行时库的 ABI 版本（打包公式同 `MB_ABI_VERSION`）。
 *  这是 UI 在加载动态库之后应当**第一个**调用的函数。
 *  返回值 != 本头文件的 `MB_ABI_VERSION` 的 MAJOR 时，UI 必须停止使用本库。 */
MB_API uint32_t MB_CALL mb_abi_version(void);

/** 可读版本字符串（如 `"1.0.0"`），写进调用方缓冲区。
 *  返回实际所需长度（含结尾 NUL）；`buf` 可为 NULL（只查询长度）。 */
MB_API size_t MB_CALL mb_abi_version_string(char* buf, size_t buf_size);

/* ============================================================================
 * 5. 日志回调（替代旧的 JNI 反射调用 logNative）
 * ========================================================================== */

typedef enum mb_log_level {
  MB_LOG_DEBUG = 0,
  MB_LOG_INFO = 1,
  MB_LOG_WARN = 2,
  MB_LOG_ERROR = 3
} mb_log_level;

/** 日志回调。`msg` 为 UTF-8、NUL 结尾，**只在回调期间有效**；`user` 原样透传，
 *  库不解引用、不释放。回调里不要再调 `mb_session_*`（避免死锁），只做日志。 */
typedef void (MB_CALL * mb_log_cb)(void* user, mb_log_level level, const char* msg);

/** 日志回调描述符（按值传字段固定；`struct_size` 用于未来尾部扩展）。 */
typedef struct mb_log_cb_desc {
  uint32_t  struct_size; /**< = sizeof(mb_log_cb_desc) */
  uint32_t  reserved0;
  mb_log_cb cb;          /**< NULL 表示注销 */
  void*     user;
} mb_log_cb_desc;

MB_STATIC_ASSERT_SIZE(mb_log_cb_desc, sizeof(void*) == 8 ? 24 : 16);

/** 注册/注销日志回调。**进程级**，与 session 无关，可在创建会话之前调用。
 *  传 NULL desc 或 desc->cb == NULL 表示注销。 */
MB_API mb_err MB_CALL mb_set_log_callback(const mb_log_cb_desc* desc);

/* ============================================================================
 * 6. 会话生命周期
 * ========================================================================== */

/** MSAA 策略（请求值）。实际生效值用 `mb_session_msaa_effective` 查询。 */
typedef enum mb_msaa {
  MB_MSAA_AUTO = 0, /**< 由库自选（当前实现：请求 4x，设备不支持则降级） */
  MB_MSAA_OFF = 1,  /**< 1x */
  MB_MSAA_4X = 2,
  MB_MSAA_8X = 3
} mb_msaa;

/** 会话创建参数。 */
typedef struct mb_session_desc {
  uint32_t    struct_size;   /**< = sizeof(mb_session_desc)；必须由调用方填 */
  uint32_t    msaa;          /**< mb_msaa */
  uint32_t    reserved0;
  uint32_t    reserved1;
  const char* app_name;      /**< 可 NULL（默认 "MagicBlocks"）；仅出现在日志里 */
  uint32_t    reserved2;
  uint32_t    reserved3;
  uint64_t    reserved4[2];
} mb_session_desc;

/* 实测：x64 与 x86 都是 48（4×u32 + 指针 + 2×u32 + 2×u64，宽度无关）。 */
MB_STATIC_ASSERT_SIZE(mb_session_desc, 48);

/** 填写 `mb_session_desc` 的推荐默认值（msaa=AUTO、app_name=NULL，其余 0）。 */
MB_API mb_session_desc MB_CALL mb_session_desc_default(void);

/** 创建会话（建设备 + 默认场景资源）。
 *
 *  @param desc 必填，`struct_size` 不对 → `MB_E_INVALID_ARG`
 *  @param out  成功时写入非 NULL 句柄；失败时写入 NULL
 *  @return MB_OK / MB_E_INVALID_ARG / MB_E_OUT_OF_MEMORY / MB_E_UNSUPPORTED /
 *          MB_E_NOT_INITIALIZED（设备建不起来；此时可用
 *          `mb_session_status_text(*out)` 读原因，若 *out 非 NULL）
 *
 *  与旧 `nativeCreate()` 的区别：旧实现**成功失败都返回非 0**，调用方只能靠猜；
 *  这里用返回值明确表达成功/失败。 */
MB_API mb_err MB_CALL mb_session_create(const mb_session_desc* desc, mb_session** out);

/** 销毁会话，释放设备/场景资源。
 *  内部保证：先与正在进行的 `mb_session_render` 串行化（最多等一帧），
 *  再释放交换链、资源、设备、surface 引用。对 NULL 是 no-op，重复调用幂等
 *  （句柄在销毁后被标记为失效，再传进来只会得到 `MB_E_INVALID_ARG`，不会 UB）。 */
MB_API mb_err MB_CALL mb_session_destroy(mb_session* s);

/** 会话是否完整可用。`MB_OK` = 可用且已 attach surface 之外的一切就绪。 */
MB_API mb_err MB_CALL mb_session_ready(const mb_session* s);

/* ============================================================================
 * 7. surface 挂载 / 卸载
 * ========================================================================== */

typedef enum mb_surface_kind {
  MB_SURFACE_ANDROID = 1, /**< native_window = ANativeWindow*；width/height = Surface 像素尺寸 */
  MB_SURFACE_WIN32 = 2,   /**< native_window = HWND */
  MB_SURFACE_XCB = 3,     /**< native_window = (void*)(uintptr_t)xcb_window_t */
  MB_SURFACE_HEADLESS = 4 /**< 离屏/自测；native_window 忽略 */
} mb_surface_kind;

/** 挂载 surface（建交换链）。
 *
 *  @param s              会话
 *  @param kind           mb_surface_kind
 *  @param native_window  平台句柄。Android 上是 `ANativeWindow*`
 *                        （Kotlin 侧 `Surface` → `ANativeWindow_fromSurface` 的结果）。
 *                        **库会自己持有/释放自己那一份引用**（Android 上
 *                        `ANativeWindow_acquire`/`_release`），调用方的引用计数自理。
 *  @param width, height  像素尺寸，必须 > 0
 *
 *  可重复调用（等价先卸再挂）。若已挂载则先内部卸载。
 *  失败时旧 surface 保持可用（不静默降级、不留下半成品交换链）。 */
MB_API mb_err MB_CALL mb_session_attach_surface(mb_session* s, uint32_t kind,
                                               void* native_window,
                                               uint32_t width, uint32_t height);

/** 卸载 surface。返回后库保证**不会再碰**该 surface，也不会再用它的交换链。 */
MB_API mb_err MB_CALL mb_session_detach_surface(mb_session* s);

/** 通知库 surface 尺寸变了（不需要卸载重挂，库内部会重建交换链）。
 *  与 `attach_surface` 的区别：不动引用计数、不动 surface 句柄。 */
MB_API mb_err MB_CALL mb_session_surface_resized(mb_session* s, uint32_t width, uint32_t height);

/* ============================================================================
 * 8. 渲染一帧
 * ========================================================================== */

/** 相机姿态。按值传、大小固定、**无指针**。
 *
 *  语义（与旧 `nativeRenderFrame` 完全一致，见 scene_renderer.h 的相机约定）：
 *    - `q[4]` 是**相机单位四元数** `(qx, qy, qz, qw)`（相机系 → 世界系）。
 *      库内部会归一化；全 0 视为非法 → `MB_E_INVALID_ARG`。
 *      约定"绕相机自身轴"累积旋转：左右拖 = 绕相机 Y，上下拖 = 绕相机 X，
 *      360° 无夹取、无万向节锁。
 *    - `scale` 缩放（双指捏合），本质是相机沿视线拉近拉远；<= 0 视为 1。
 *    - `pan[2]` 世界空间平移 `(x, y)`，由 UI 按"屏幕像素 / 当前可视宽度"换算好再传。
 */
typedef struct mb_pose {
  float q[4];
  float scale;
  float pan[2];
} mb_pose;

MB_STATIC_ASSERT_SIZE(mb_pose, 28);

/** 默认姿态：q = 单位四元数 (0,0,0,1)、scale = 1、pan = (0,0)。 */
MB_API mb_pose MB_CALL mb_pose_default(void);

/** 渲染并呈现一帧（无 CPU 回读）。
 *
 *  @return MB_OK            已呈现
 *          MB_E_NO_SURFACE  未挂载 surface，未做任何事
 *          MB_E_OUT_OF_DATE 交换链已内部重建，本帧丢弃（UI 忽略，下一帧自然好）
 *          MB_E_SURFACE_LOST surface 已死（UI 应停止渲染循环，等新的 surface 事件）
 *          MB_E_DEVICE_LOST GPU 丢失（需销毁重建会话）
 *          MB_E_INVALID_ARG 句柄或 pose 非法
 *          MB_E_INTERNAL    其它错误，细节见 `mb_session_status_text`
 *
 *  线程：可在 UI 线程或**单一**渲染线程上调用。库内部对每个 GPU 等待都有
 *  上界，超时返回错误而不是永久阻塞。 */
MB_API mb_err MB_CALL mb_session_render(mb_session* s, mb_pose pose);

/* ============================================================================
 * 9. MSAA
 * ========================================================================== */

/** 设置**请求**的 MSAA 档位。生效时机：下一次 `mb_session_attach_surface`。
 *  设备不支持时降级（不失败），实际值用 `mb_session_msaa_effective` 查。
 *  传入越界值 → `MB_E_INVALID_ARG`（不静默改写）。 */
MB_API mb_err MB_CALL mb_session_set_msaa(mb_session* s, mb_msaa msaa);

/** 取**实际生效**的采样数（1/2/4/8/16）。
 *  UI 必须显示这个值而不是请求值 —— 否则会出现"点了 8x 实际 4x"的假象。 */
MB_API mb_err MB_CALL mb_session_msaa_effective(const mb_session* s, uint32_t* out_samples);

/** 取设备报告的受支持档位（升序，至少含 1）。
 *
 *  两段式调用：
 *    - `out_samples == NULL` 或 `capacity == 0` → 只写 `*out_count`（所需个数）；
 *    - 否则写入最多 `capacity` 个，并写 `*out_count` = 实际写入个数。
 *  若 `*out_count > capacity`，说明缓冲区不够，调用方应扩容重试。 */
MB_API mb_err MB_CALL mb_session_msaa_supported(const mb_session* s,
                                               uint32_t* out_samples,
                                               size_t capacity,
                                               size_t* out_count);

/* ============================================================================
 * 10. 模型
 * ========================================================================== */

#define MB_MODEL_NAME_MAX 64  /**< name 字段容量（含 NUL） */
#define MB_MODEL_MSG_MAX  128 /**< message 字段容量（含 NUL） */
#define MB_MODEL_PATH_MAX 252 /**< path 字段容量（含 NUL） */

/** 一个预设模型条目。定长、无指针、位宽无关（464 字节）。 */
typedef struct mb_model_entry {
  char     name[MB_MODEL_NAME_MAX];  /**< UI 只显示它 */
  char     path[MB_MODEL_PATH_MAX];  /**< 完整路径（日志/排错用；UI 不要解析） */
  char     message[MB_MODEL_MSG_MAX];/**< ok == 0 时的原因 */
  uint64_t bytes;                    /**< 文件字节数 */
  uint32_t ok;                       /**< 1 = 扫描阶段可用，0 = 不可用 */
  uint32_t reserved0;
} mb_model_entry;

MB_STATIC_ASSERT_SIZE(mb_model_entry, 464);

/** 枚举 `directory` 下的预设模型（不递归）。
 *
 *  **不依赖会话**：`s` 可为 NULL（纯目录扫描）。传非 NULL 则共享其日志设置。
 *
 *  "目录不存在/不可读" 返回 `MB_E_NOT_FOUND` / `MB_E_IO`（**不是空列表**）；
 *  "空目录" 返回 `MB_OK` 且 `*out_count == 0`。这两件事必须在错误码上可区分。
 *
 *  成功时 `*out` 非 NULL，必须由调用方 `mb_model_list_free`。 */
MB_API mb_err MB_CALL mb_session_scan_models(const mb_session* s, const char* directory,
                                            mb_model_list** out);

/** 列表条目数。对无效句柄返回 0。 */
MB_API size_t MB_CALL mb_model_list_count(const mb_model_list* l);

/** 取第 `index` 个条目（**按值写进调用方结构体**，不返回内部指针）。
 *  `index` 越界 → `MB_E_INVALID_ARG` / `MB_E_NOT_FOUND`。 */
MB_API mb_err MB_CALL mb_model_list_get(const mb_model_list* l, size_t index,
                                        mb_model_entry* out);

/** 释放列表。对 NULL 是 no-op，重复调用幂等。 */
MB_API mb_err MB_CALL mb_model_list_free(mb_model_list* l);

/* ---------------------------------------------------------------------------
 * 模型列表的**文本线格式**(JNI 层 `nativeListModels` 交给 Kotlin 的那个字符串)
 * ---------------------------------------------------------------------------
 * 这不是给人看的文档，而是机器可以引用的契约：JNI 层、Kotlin 侧、桌面语料三方
 * 都按这一组常量走，跨语言的那一份在 `tools/capi_protocol/protocol.json`。
 *
 *   err=<MB_E_*>           <- 恒有，且是第一行（成功时是 MB_OK）
 *   <name>                     每个模型恰好 MB_PROTOCOL_TOKENS_PER_MODEL 行，
 *   <path>                     行序 = mb_protocol_field 的顺序，每行一个字段
 *   <bytes>
 *   <ok>       "0" / "1"
 *   <message>
 *
 * 语义上必须保住的两条（t3 特意保留，Kotlin 侧必须能分别显示）：
 *   - 空目录        -> 首行 MB_OK，且**没有条目**；
 *   - 目录不存在    -> 首行 MB_E_NOT_FOUND。
 * 这两件事塌成同一个状态，UI 就没法说清"这里本来没东西"和"路径读不到"。
 *
 * 为什么首行恒带 `err=`：旧格式用 `"ok"` / `"err\t原因"` 混在同一行，成功与失败
 * 的**行结构都不一样**，解析器必须先判形态再切字段，且原因里有制表符就拆错。
 * 现在首行永远是同一个 key=value 形态，失败时直接没有数据区。
 * ------------------------------------------------------------------------- */
#define MB_PROTOCOL_STATUS_PREFIX "err="    /**< 首行前缀 */
#define MB_PROTOCOL_ERR_OK "MB_OK"          /**< 首行前缀后表示扫描成功 */
#define MB_PROTOCOL_LINE_SEP_STR "\n"      /**< 行分隔（字符串形态，拼文本用） */
#define MB_PROTOCOL_FIELD_SEP '\t'          /**< 字段分隔（每行一个字段时它不该出现） */
#define MB_PROTOCOL_TOKENS_PER_MODEL 5      /**< 每个模型的字段行数 */
#define MB_PROTOCOL_SANITIZE_CHARS "\r\n\t" /**< 定长字段里这些字符被换成空格 */
#define MB_PROTOCOL_SANITIZE_CHAR ' '

/** 文本线格式的字段顺序（下标即协议位置）。 */
typedef enum mb_protocol_field {
  MB_PROTOCOL_FIELD_NAME = 0,
  MB_PROTOCOL_FIELD_PATH = 1,
  MB_PROTOCOL_FIELD_BYTES = 2,
  MB_PROTOCOL_FIELD_OK = 3,
  MB_PROTOCOL_FIELD_MESSAGE = 4
} mb_protocol_field;

/** `nativeListModels` 返回文本的**协议版本**（当前 1）。
 *
 *  与 `mb_abi_version()` 是**两件事**：
 *    - ABI 版本管的是 26 个入口的名字/签名/结构体布局；
 *    - 协议版本管的是上面那一个字符串里的标记、分隔与字段顺序。
 *  只新增一个函数 → ABI MINOR+1、协议不动；只改 sanitize 规则 → 协议+1、ABI 不动。
 *  两件事分开数，是因为"谁必须跟着更新"完全不同：ABI 变了 UI 必须重编，
 *  协议变了只需要解析器跟上。
 *
 *  UI 在解析前先比这个值：不相等就不要猜格式（文本协议没有"尾部追加字段"
 *  这种扩展位，猜错的代价是静默显示错数据）。 */
MB_API uint32_t MB_CALL mb_model_list_protocol_version(void);

/** 切换到 `path` 指定的 OBJ。`name` 只用于日志/状态显示（可 NULL）。
 *
 *  失败时**渲染保持原状**：不回落内置立方体、不留半成品几何。
 *  失败原因用 `mb_session_status_text` 读。 */
MB_API mb_err MB_CALL mb_session_load_model(mb_session* s, const char* path, const char* name);

/** 切回内置立方体并释放模型资源。 */
MB_API mb_err MB_CALL mb_session_clear_model(mb_session* s);

/* ============================================================================
 * 11. 状态与性能
 * ========================================================================== */

/** 把状态文本写进调用方缓冲区（UTF-8、NUL 结尾）。
 *
 *  `buf == NULL` 或 `buf_size == 0` → 只返回所需长度（含 NUL）。
 *  `buf` 太短 → 写入 `buf_size - 1` 字节 + NUL，仍返回所需长度供扩容重试。
 *  **返回的文本仅供显示，格式不保证**：需要结构化数据请用 `mb_session_perf` / `mb_err`。
 *
 *  返回 0 表示句柄无效。 */
MB_API size_t MB_CALL mb_session_status_text(const mb_session* s, char* buf, size_t buf_size);

/** 性能快照。按值返回、无分配、可从任意线程读取（内部原子快照）。 */
typedef struct mb_perf_snapshot {
  uint32_t struct_size;      /**< = sizeof(mb_perf_snapshot) */
  uint32_t msaa_effective;   /**< 当前生效采样数 */
  float    fps;              /**< 成功呈现帧数 / 统计窗口秒数 */
  float    frame_ms_avg;     /**< 每帧 render 平均耗时（毫秒） */
  float    frame_ms_max;     /**< 统计窗口内单帧最大耗时（毫秒） */
  float    render_fail_ratio;/**< 失败帧占比，0.0 ~ 1.0 */
  uint64_t frames_total;     /**< 累计成功呈现帧数 */
  uint64_t reserved0;
} mb_perf_snapshot;

MB_STATIC_ASSERT_SIZE(mb_perf_snapshot, 40);

/** 读性能快照。句柄无效时返回全 0 的快照（`struct_size` 仍正确填写）。 */
MB_API mb_perf_snapshot MB_CALL mb_session_perf(const mb_session* s);

#if defined(__cplusplus)
} /* extern "C" */
#endif

/* 恢复调用方原本的 `MB_OK`(Windows 平台宏),见 §0 的说明。
 *
 * 这里用"自己重新定义成平台原文"而不是 `#pragma push_macro/pop_macro`:
 *   - pop_macro 在调用方那侧会以 warning C4005 报"重定义",给 UI 的构建凭空多一条警告;
 *   - 而按 WinUser.h 的原文 `0x00000000L` 重新定义,值、类型(long)、文本后缀都对得上,
 *     `MessageBox(...) == MB_OK` 之类别处代码行为完全等价。
 *
 * 只有在进来时 `MB_OK` **已存在**(= 调用方是 Windows 平台)才恢复,标记用
 * `MB_CAPI_RESTORED_MB_OK`。 */
#if defined(MB_CAPI_RESTORED_MB_OK)
#  undef MB_CAPI_RESTORED_MB_OK
#  if !defined(MB_OK)
#    define MB_OK 0x00000000L  /* 与 WinUser.h 的 MessageBox 返回值宏逐字一致 */
#  endif
#endif

#endif /* MAGICBLOCKS_MB_CAPI_H */
