package com.magicblocks.pad

/**
 * UI 侧对 SDK ABI 的**全部知识**—— 就这一个文件。
 *
 * ============================================================================
 * 为什么这些数字要手写在 Kotlin 里,而不是从 native 读
 * ============================================================================
 * 版本校验的全部意义就是"**不信任**运行时库"。如果 `requiredVersion` 是问库要的,
 * 那么语义会退化成"我要求 = 它给的",永远相等,校验恒真 —— 一条永远绿的断言
 * 与没有断言等价(和 t3 门控里"永远匹配不上的正则"是同一个病)。
 *
 * 所以:需要的版本是**编译期常量**(跟着 UI 的代码一起发布),
 * 实际版本是**运行时值**(跟着 .so 一起发布)。两者对不上才是"UI 和 SDK 不是一套"。
 *
 * ============================================================================
 * 与 mb_capi.h 的对应关系(改动规则)
 * ============================================================================
 * `MagicBlocks/include/magicblocks/mb_capi.h` §0 是 SDK 侧的定义,这里是 UI 侧镜像。
 * 两边都是**人写的**,所以必须有东西盯着它们一致:
 *   - `tools/capi_protocol/protocol.json` 的 `abi` 段是跨语言的那份声明;
 *   - `tools/capi_protocol/gen_abi_table.py --check` 会把本文件的每个常量
 *     与 protocol.json 逐项比对,任何一项不一致直接失败。
 * 改动顺序永远是:改 mb_capi.h → 改 protocol.json → 改本文件 → 跑 --check。
 */
object MbAbi {

    // ------------------------------------------------------------------ 版本
    //
    // 打包公式与 mb_capi.h §0 **逐字对应**:
    //     MB_ABI_VERSION = MAJOR * 10000 + MINOR * 100 + PATCH
    // MAJOR 是"破坏性变更"位(删/改函数签名或结构体字段、改语义),
    // MINOR 是"只增不改"位(新增函数、结构体尾部追加字段),
    // PATCH 对 UI 没有可观测意义(纯实现修复,ABI 不变)。

    /** 破坏性变更位。**不相等**就绝不能再调其它任何入口。 */
    const val MAJOR = 1

    /** 只增不改位。UI 要求"至少"到这个值。 */
    const val MINOR = 0

    /** 纯修复位。UI 不校验它(见 [compatibleWith])。 */
    const val PATCH = 0

    // 打包位宽。**必须是字面量**:Kotlin 的 `const val` 只接受编译期常量,
    // 不能引用另一个 `const val`(试过 `MAJOR * BIT_STRIDE_MAJOR`,
    // 编译器直接 CONST_VAL_WITH_NON_CONST_INITIALIZER)。所以下面两个常量之间
    // 的关系由 `tools/capi_protocol/gen_abi_table.py --check` 来保证,而不是靠写法。
    const val BIT_STRIDE_MAJOR = 10000
    const val BIT_STRIDE_MINOR = 100

    /** 把 (major, minor, patch) 打成与 `mb_abi_version()` 同构的整数。 */
    fun pack(major: Int, minor: Int, patch: Int): Int =
        major * BIT_STRIDE_MAJOR + minor * BIT_STRIDE_MINOR + patch

    /**
     * UI 运行所**必需**的最低 ABI 版本 = 打包后的 (MAJOR, MINOR, PATCH=0)。
     *
     * 注意这是"最低",不是"精确":([compatibleWith]) 只要求 MAJOR 相等、
     * 且 MINOR 不小于 [MINOR]。所以 SDK 只是加了新函数(MINOR+1)时,
     * UI **不需要**跟着更新 —— 这正是"SDK 随便重构、UI 不受影响"的那半边承诺;
     * 只有当 MAJOR 变了(真的删/改了 UI 正在用的东西),UI 才必须重发。
     *
     * (同样只能是字面量:它是 `const val`,见上面 BIT_STRIDE_* 处的说明。)
     */
    const val requiredVersion: Int = 10000

    /** 打包值里 MAJOR 位的乘数(10000 × 100):`majorOf` 用它切位。 */
    const val BIT_STRIDE_MAJOR_FULL = BIT_STRIDE_MAJOR * BIT_STRIDE_MINOR

    /** 从打包值里切出 MAJOR 位。 */
    fun majorOf(packed: Int): Int = packed / BIT_STRIDE_MAJOR_FULL

    /** 从打包值里切出 MINOR 位。 */
    fun minorOf(packed: Int): Int = (packed / BIT_STRIDE_MINOR) % BIT_STRIDE_MINOR

    /** 打包值 → 给人看的 "1.0.0"(仅用于错误信息)。 */
    fun format(packed: Int): String {
        val major = majorOf(packed)
        val minor = minorOf(packed)
        val patch = packed % BIT_STRIDE_MINOR
        return "$major.$minor.$patch"
    }

    /**
     * 运行时版本 `runtime` 能否服务这份 UI。
     *
     * 判据(vs 用 `runtime < requiredVersion` 单条比大小的理由见下):
     *   1. MAJOR 必须**相等**。小了 = 库太旧、UI 用到的东西它没有;
     *      大了 = 库已经做破坏性变更,UI 手里的调用约定可能已被删改。
     *      两种情况都不能继续调(头文件 §0 第 40 行就是这么要求的)。
     *   2. MINOR 必须 **>=** 本文件的 MINOR。库比 UI 新(把新函数加在尾部)
     *      是兼容的,不该拦。
     *
     * 为什么不用 `runtime < requiredVersion`:那会把"库比 UI 新"也判成错。
     * 而"库比 UI 新一个 MINOR"恰恰是要允许的 —— 否则 SDK 每加一个入口,
     * 已经在手机上的 UI 就全部罢工,那等于每次 SDK 发版都强制 UI 发版,
     * 和本项目的目标(UI 与 SDK 更新解耦)正好相反。
     */
    fun compatibleWith(runtime: Int): Boolean {
        val rm = majorOf(runtime)
        if (rm != MAJOR) return false
        return minorOf(runtime) >= MINOR
    }

    /** 比对结论:能服务就是 null,否则是一句"给人看、能直接贴到 HUD 上"的话。 */
    fun mismatchDetail(runtime: Int): String? {
        if (runtime == 0) {
            return "SDK 没有报告 ABI 版本(mb_abi_version() 返回 0):" +
                "加载到的不是 MagicBlocks 引擎库,或导出表残缺。" +
                "UI 需要 ${format(requiredVersion)}($requiredVersion)。"
        }
        if (compatibleWith(runtime)) return null
        val why = if (majorOf(runtime) != MAJOR) {
            "MAJOR 不一致:UI 是按 ${format(requiredVersion)} 编译的," +
                "SDK 运行时是 ${format(runtime)}。声明为破坏性变更的版本差异," +
                "界面上的字段/入口可能已经被删改。"
        } else {
            "MINOR 太低:UI 需要至少 ${format(requiredVersion)}," +
                "SDK 运行时只有 ${format(runtime)}(缺 UI 依赖的新入口)。"
        }
        return "$why 请让 UI 与 SDK 使用同一套产物" +
            "(重新打包 APK,或替换 app/src/main/jniLibs 下的 libmagicblocks.so)。"
    }

    // ------------------------------------------------------------------ 协议
    //
    // 这里放的是**文本协议**的版本号,与上面的 ABI 版本**是两件事**:
    //   ABI 版本管 26 个 C 入口的名字/签名/结构体;
    //   协议版本管 nativeListModels 那一个字符串里的标记/分隔/字段顺序。
    // 只加一个新函数是 ABI MINOR+1、协议不变;只改 sanitize 规则是协议+1、ABI 不变。
    // 两件事分开数,是因为它们的"谁必须跟着更新"完全不同。

    /**
     * UI 能解析的 `nativeListModels` 返回文本版本(见 [MbAbiProtocol])。
     *
     * 与 [nativeModelListProtocolVersion] 报告的值必须**相等**才解析 —— 文本格式没有
     * 向后兼容的余地(它没有"尾部追加字段"这种扩展位),所以这里比的是相等而不是 >=。
     */
    const val MODEL_LIST_PROTOCOL_VERSION = 1
}
