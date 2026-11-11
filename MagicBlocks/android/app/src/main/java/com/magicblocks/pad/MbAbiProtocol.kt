package com.magicblocks.pad

/**
 * `nativeListModels` 返回文本的**唯一**解析实现。
 *
 * ============================================================================
 * 格式(与 SDK 侧同源,见 tools/capi_protocol/protocol.json)
 * ============================================================================
 * ```
 * err=<MB_E_*>            ← 恒有,且是第一行
 * <name>                  ┐
 * <path>                  │
 * <bytes>                 │ 每个模型 5 行,每行恰好一个字段
 * <ok>      0 / 1         │ 行序 = MbAbiProtocol 里的 field_* 常量
 * <message>               ┘
 * ...
 * ```
 * 两段之间没有分隔行,所以"每模型固定 5 行"就是全部结构信息。
 *
 * ============================================================================
 * 为什么这个文件要和 jni_bridge.cpp 严格对齐,以及为什么会有过一次事故
 * ============================================================================
 * 这套格式原来是"两侧各写一份约定":C++ 拼字符串,Kotlin 切字符串,没有任何东西
 * 约束它们一致。t3 把首行从 `"ok"` 改成 `"err=MB_E_*"` 之后,Kotlin 侧仍在比对
 * `lines[0] != "ok"`,于是**每次扫描都无条件失败**;`removePrefix("err\t")` 也去不掉
 * `err=` 的前缀,界面上显示的是字面量 `err=MB_OK`(宣称失败却写着成功)。
 * 桌面全绿 —— 冒烟只打到 C ABI 层,没人验 Kotlin 侧解析。
 *
 * 现在的防守是三条,都必须在桌面上能跑:
 *   1. **不再有字面量**:首行前缀、字段分隔、行分隔全部来自本文件的常量,
 *      而本文件的常量由 `gen_abi_table.py --check` 钉死在 protocol.json 上
 *      (Kotlin 里出现协议字面量会直接判红);
 *   2. **同一批字节喂给三方**:`MagicBlocks/tests/capi_protocol/golden_cases.h`
 *      里是生成的语料,C++ 的拼装结果是它,本文件解析的输入也是它,
 *      桌面程序还用 JVM 反射直接调 [parseModelList](真实编译产物,不是副本);
 *   3. **版本显式**:协议版本由 `nativeModelListProtocolVersion()` 单独报告,
 *      对不上就明确报错,而不是"猜一个格式解析下去"。
 */
object MbAbiProtocol {

    // ---------------------------------------------------------------- 格式标记
    //
    // ⚠ 这三个常量是"协议"本身。改它们必须同时改
    //   tools/capi_protocol/protocol.json,否则 `gen_abi_table.py --check` 会红。

    /** 首行前缀。**恒有**,即使成功(此时后面跟 [STATUS_OK])。 */
    const val STATUS_PREFIX = "err="

    /** 首行前缀后表示"扫描本身成功"的标记。注意 `MB_OK` 是**错误码名**,不是"ok"。 */
    const val STATUS_OK = "MB_OK"

    /** 行分隔符。 */
    const val LINE_SEP = "\n"

    /** 字段分隔符。**当前协议下字段各占一行**,所以解析结果里不该出现它;
     *  留着是因为"字段里混进分隔符"本身就是一个该报告的损坏信号。 */
    const val FIELD_SEP = "\t"

    // ---------------------------------------------------------------- 字段序号
    //
    // 名字 + 序号,替代"f[2].toLongOrNull()"那种魔法下标。顺序就是协议顺序。

    const val FIELD_NAME = 0
    const val FIELD_PATH = 1
    const val FIELD_BYTES = 2
    const val FIELD_OK = 3
    const val FIELD_MESSAGE = 4

    /** 每个模型占的行数(= 字段数)。协议里没有分隔行,所以这个数是硬约束。 */
    const val TOKENS_PER_MODEL = 5

    /**
     * 本解析器认识的协议版本。
     *
     * ⚠ 这里只能是**字面量**:Kotlin 的 `const val` 不接受引用另一个 `const val`。
     * 所以它与 [MbAbiProtocol] 之外那份声明(`MbAbi.MODEL_LIST_PROTOCOL_VERSION`,
     * 用来和 `nativeModelListProtocolVersion()` 比对)之间的一致性不能靠语言保证,
     * 改由 `tools/capi_protocol/gen_abi_table.py --check` 钉住(两处都必须等于
     * protocol.json 的 `protocol_version`)。
     */
    const val PROTOCOL_VERSION = 1

    /**
     * 解析器标识,形如 `MbAbiProtocol/1`。
     *
     * 为什么需要一个"自报家门"的字符串:协议版本号在 Kotlin 里是个**常量**,而常量
     * 在 .class 里是 final 字段,单靠反射看不出它是不是真被这份源码编译出来的。
     * 但解析**成功时的诊断串**里会带上它 —— 于是桌面校验程序可以真的调一次解析,
     * 从返回值里读出这个标识,再与 `tools/capi_protocol/protocol.json` 的
     * `parser_id` 比对:加载到旧 class 文件、或者两份常量漂了,都会当场红。
     */
    const val PARSER_ID = "MbAbiProtocol/1"

    /** 错误码名 → 给人看的中文原因。**只在这里维护**,UI 别的文件不要再猜错误码含义。 */
    private val ERR_REASONS = mapOf(
        "MB_OK" to "扫描成功",
        "MB_E_NOT_FOUND" to "目录不存在",
        "MB_E_IO" to "目录不可读(权限或 IO 错误)",
        "MB_E_INVALID_ARG" to "参数非法",
        "MB_E_NOT_INITIALIZED" to "引擎尚未初始化完成",
        "MB_E_OUT_OF_MEMORY" to "内存不足",
        "MB_E_UNSUPPORTED" to "当前后端不支持",
        "MB_E_INTERNAL" to "引擎内部错误",
    )

    /**
     * 解析 [raw]。
     *
     * 三种结果严格区分(见 [ModelScan]):
     *   - [ModelScan.Ok]      扫描成功。`items.isEmpty()` = **目录里没有 OBJ**,这是合法状态;
     *   - [ModelScan.Failed]  扫描失败(目录不存在 / 不可读 / 返回文本损坏);
     *   - 抛异常             **不**在契约里 —— 任何输入都必须得到一个结果。
     */
    fun parseModelList(raw: String): ModelScan {
        if (raw.isEmpty()) {
            return ModelScan.Failed("引擎返回了空字符串(nativeListModels 未产出任何内容)")
        }
        // 只按 LINE_SEP 切:Windows/Android 侧字段里的 \r 已被 SDK 换成空格,
        // 这里再做一次归一只是为了容忍手工构造的输入。
        val lines = raw.split(LINE_SEP).map { it.trimEnd('\r') }
        val head = lines[MbAbiProtocol.FIELD_NAME]
        if (!head.startsWith(STATUS_PREFIX)) {
            return ModelScan.Failed(
                "返回文本的格式不符合协议 v$PROTOCOL_VERSION:" +
                    "首行应以 \"$STATUS_PREFIX\" 开头,实际是 \"${head.take(40)}\"。"
            )
        }
        // removePrefix 而非 substring(STATUS_PREFIX.length):此处 startsWith 已经保证了
        // 前缀存在,两者等价 —— 用 removePrefix 是为了让"只在真有前缀时才切"这件事
        // **写在代码里**,而不是靠上面那个 if 的隐含前提。以后谁把 if 改了(比如为了
        // 兼容别的首行放宽),也不会在下面炸出一个 IndexOutOfBounds。
        val err = head.removePrefix(STATUS_PREFIX).trim()
        if (err != STATUS_OK) {
            val known = ERR_REASONS[err]
            val reason = if (known != null) "$known($err)" else "未登记的错误码 $err"
            return ModelScan.Failed("扫描失败:$reason")
        }

        // 重要:**不能** `filter { it.isNotEmpty() }`。
        // 每模型 5 行,其中 message 允许为空串 —— 一条"解析成功且没有告警"的记录,
        // 它的第 5 行就是空行;把空行滤掉会把 5 行/条悄悄变成 4 行/条,
        // 于是整表错位(名字列跑进路径列),而条数一除居然还整得开,最坏的情况是静默出错。
        // 协议文本以 LINE_SEP 结尾,split 后末尾必然多一个空元素:只去掉这**一个**尾元素。
        val tail = if (lines.isNotEmpty() && lines.last().isEmpty()) lines.dropLast(1) else lines
        val body = tail.drop(1)
        if (body.size % TOKENS_PER_MODEL != 0) {
            // 字段不足以说明数据损坏时**明确诊断**,而不是静默把尾巴丢掉 ——
            // 静默丢弃会让"列表少了一个模型"看起来像"目录里本来就没它"。
            return ModelScan.Failed(
                "模型列表数据损坏:条目行数 ${body.size} 不是 $TOKENS_PER_MODEL 的整数倍" +
                    "(协议 v$PROTOCOL_VERSION 每模型 $TOKENS_PER_MODEL 行)。" +
                    "这通常说明 SDK 侧的输出格式与 UI 侧的解析不一致。"
            )
        }

        val items = ArrayList<ModelItem>(body.size / TOKENS_PER_MODEL)
        var i = 0
        var index = 0
        while (i < body.size) {
            val name = body[i + FIELD_NAME]
            val path = body[i + FIELD_PATH]
            val bytes = body[i + FIELD_BYTES].toLongOrNull()
            val okFlag = body[i + FIELD_OK]
            val message = body[i + FIELD_MESSAGE]
            val broken = when {
                path.isEmpty() -> "path 字段为空"
                bytes == null -> "bytes 字段不是整数(\"${body[i + FIELD_BYTES]}\")"
                okFlag != "0" && okFlag != "1" -> "ok 字段不是 0/1(\"$okFlag\")"
                // SDK 侧保证字段里不出 TAB/换行(换行已被换成空格);这里再验一遍是
                // **防御性**的:上一版 bug 正是"字段里一个 TAB 就把一行拆成两行"。
                // message 也要验 —— 它是唯一一个允许为空的自由文本字段。
                name.contains(FIELD_SEP) -> "name 字段里混进了字段分隔符"
                path.contains(FIELD_SEP) -> "path 字段里混进了字段分隔符"
                message.contains(FIELD_SEP) -> "message 字段里混进了字段分隔符"
                else -> null
            }
            if (broken != null) {
                return ModelScan.Failed(
                    "模型列表第 ${index + 1} 条损坏:$broken。" +
                        "这通常说明 SDK 侧的输出格式与 UI 侧的解析不一致。"
                )
            }
            items.add(
                ModelItem(
                    // 名字为空不算致命:path 是权威的,名字只用于显示。
                    name = name.ifEmpty { path.substringAfterLast('/') },
                    path = path,
                    bytes = bytes ?: 0L,
                    ok = okFlag == "1",
                    message = message
                )
            )
            i += TOKENS_PER_MODEL
            index++
        }
        return ModelScan.Ok(items)
    }

    /**
     * 桌面校验入口(真机路径上没有人调它;`gen_abi_table.py --check` 会确认这一点 ——
     * 源码里除本文件的定义外,`selftestParse` / `signatureOf` 不得再出现第二次)。
     *
     * 为什么要它:`MbRenderer.kt` 里有一堆 `android.*` / Compose 依赖,桌面 JVM 装不进来,
     * 所以那部分解析逻辑没法在桌面上直接验。于是把**纯解析**单独放在本文件里
     * (不 import 任何 android.*),桌面程序就能用 JVM 反射直接调这个函数,
     * 喂生成好的语料,验的是**真实编译产物**而不是一份抄写。
     *
     * 返回 `(kind, detail)`,kind ∈ {"ok", "failed", "throw"}:
     *   - "ok"      → detail 是「条目数,外加一条**规范化签名**」:每条的 `名字|路径|字节|
     *                 ok标志|消息`,再用 `/` 连起来。只报个数是不够的 —— 数对了但字段
     *                 错位(比如 name 里是路径)照样是解析器坏了,而 `ok\t2` 看不出来。
     *                 签名由 [signatureOf] 生成,校验程序那边用同一规则重算,两边逐字符比。
     *                 签名末尾带 [PARSER_ID]:核对的是"这份解析器"而不是"某个解析器"。
     *   - "failed"  → detail 是 [ModelScan.Failed.reason]
     *   - "throw"   → detail 是异常类型 + 消息(契约上不该出现,所以能红)
     *
     * 分隔符用 `|` 而不是协议自己的 TAB/换行:这里是**校验程序的私有格式**,
     * 不是返回文本的格式,不能让人误以为它也是协议的一部分。
     */
    @JvmStatic
    fun selftestParse(raw: String): String {
        return try {
            // 不能写 `when (val r = parseModelList(raw)) { is ModelScan.Ok -> ...;
            // is ModelScan.Failed -> ... }` 之类的"非穷尽 when":Kotlin 在那种写法上
            // 会插入 `throw NoWhenBranchMatchedException()` 分支,而那个类在
            // kotlin-stdlib 里 —— 桌面校验的 classpath 一旦缺 stdlib 就变成
            // NoClassDefFoundError,报错落在**调用点**上,极难归因。
            val r = parseModelList(raw)
            when (r) {
                is ModelScan.Ok -> "ok\t${r.items.size}\t${signatureOf(r.items)}"
                is ModelScan.Failed -> "failed\t${r.reason}"
                else -> "failed\t解析器返回了未知的 ModelScan 子类型: ${r.javaClass.name}"
            }
        } catch (t: Throwable) {
            "throw\t${t.javaClass.name}: ${t.message}"
        }
    }

    /** [selftestParse] 的规范化签名。规则必须与 `check_kotlin_abi.py` 的
     *  `expected_signature()` 逐字一致 —— 那边是独立实现的同一套规则,不是抄这个函数。 */
    @JvmStatic
    fun signatureOf(items: List<ModelItem>): String {
        val body = items.joinToString("/") {
            "${it.name}|${it.path}|${it.bytes}|${if (it.ok) "1" else "0"}|${it.message}"
        }
        return "$PARSER_ID#{${if (items.isEmpty()) "empty" else body}}"
    }
}

/** 一个预设模型条目(由引擎目录扫描给出,UI 只负责显示 name)。 */
data class ModelItem(
    val name: String,
    val path: String,
    val bytes: Long,
    val ok: Boolean,
    val message: String
)

/** 预设模型目录的扫描结果。"空目录"和"目录读不到"是两件事,必须分开报。 */
sealed class ModelScan {
    data class Ok(val items: List<ModelItem>) : ModelScan()
    data class Failed(val reason: String) : ModelScan()
}
