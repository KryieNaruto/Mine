// 桌面签名/协议校验的 **Java 端**(由 check_kotlin_abi.py 用 javac 现场编译再用 java 运行)。
//
// 为什么不在 Python 里用 ctypes 直接调 libjvm:那条路要手工维护整个 JNI 函数表的下标。
// JNI 里有大量预留槽位(4..23、30..32、60..64、175..178 …),数错一格不会报错,
// 而是**静默调到别的函数上** —— 实测就踩过一次:把 GetMethodID(33)写成了
// GetStaticMethodID(113),症状是 `NoSuchMethodError: selftestParse`,看起来像
// "类里没这个方法",实际是查错了方法表。用真 Java 就不存在这个问题。
//
// 输入(标准输入,一个 JSON):{"classpath": [...], "cases": [{key,text}, ...]}
// 输出(标准输出,一个 JSON):
//   {
//     "methods":  {"<类名>": {"<方法名>": {"ret": "...", "params": [...]}}},
//     "values":   {"<类名>.<字段>": 值, "selftest.<case>": "..."},
//     "errors":   [...]
//   }
//
// 注意:本文件**不依赖仓内任何代码**,连包名都不引用 —— 反射出来的就是真实的 .class。

import java.util.*;

public final class Probe {
    private static String d(Class<?> c) {
        if (c.isArray()) return c.getName().replace('.', '/');
        switch (c.getName()) {
            case "boolean": return "Z";
            case "byte": return "B";
            case "char": return "C";
            case "short": return "S";
            case "int": return "I";
            case "long": return "J";
            case "float": return "F";
            case "double": return "D";
            case "void": return "V";
            default: return "L" + c.getName().replace('.', '/') + ";";
        }
    }

    public static void main(String[] args) throws Exception {
        String payload = new String(System.in.readAllBytes(), "UTF-8");
        Map<String, Object> in = Json.parseObject(payload);
        @SuppressWarnings("unchecked")
        List<String> classpath = (List<String>) in.get("classpath");
        @SuppressWarnings("unchecked")
        List<Map<String, String>> cases = (List<Map<String, String>>) in.get("cases");

        Map<String, Object> out = new LinkedHashMap<>();
        Map<String, Object> methods = new LinkedHashMap<>();
        Map<String, Object> values = new LinkedHashMap<>();
        List<String> errors = new ArrayList<>();
        out.put("methods", methods);
        out.put("values", values);
        out.put("errors", errors);

        ClassLoader cl = Probe.class.getClassLoader();
        Class<?> renderer;
        try {
            renderer = Class.forName("com.magicblocks.pad.MbRenderer", true, cl);
        } catch (Throwable t) {
            errors.add("加载 MbRenderer 失败: " + t);
            System.out.println(Json.write(out));
            return;
        }
        methods.put(renderer.getName(), methodTable(renderer));

        // 静态常量:读的是编译器**真的**写进 .class 的值。
        String abi = "com.magicblocks.pad.MbAbi";
        for (String f : new String[]{"MAJOR", "MINOR", "PATCH", "BIT_STRIDE_MAJOR",
                "BIT_STRIDE_MINOR", "BIT_STRIDE_MAJOR_FULL", "requiredVersion",
                "MODEL_LIST_PROTOCOL_VERSION"}) {
            values.put("MbAbi." + f, staticField(abi, f));
        }
        String proto = "com.magicblocks.pad.MbAbiProtocol";
        for (String f : new String[]{"STATUS_PREFIX", "STATUS_OK", "LINE_SEP", "FIELD_SEP",
                "PARSER_ID"}) {
            values.put("MbAbiProtocol." + f, staticField(proto, f));
        }
        for (String f : new String[]{"FIELD_NAME", "FIELD_PATH", "FIELD_BYTES", "FIELD_OK",
                "FIELD_MESSAGE", "TOKENS_PER_MODEL", "PROTOCOL_VERSION"}) {
            values.put("MbAbiProtocol." + f, staticField(proto, f));
        }

        // 真的调一次解析:喂生成好的语料 + 校验器自造的病态输入。
        try {
            Class<?> pc = Class.forName(proto, true, cl);
            java.lang.reflect.Method m = pc.getMethod("selftestParse", String.class);
            for (Map<String, String> c : cases) {
                String key = c.get("key");
                String text = c.get("text");
                if (text == null) text = "";
                try {
                    values.put("selftest." + key, String.valueOf(m.invoke(null, text)));
                } catch (Throwable t) {
                    Throwable cause = (t.getCause() != null) ? t.getCause() : t;
                    values.put("selftest." + key,
                            "throw\t" + cause.getClass().getName() + ": " + cause.getMessage());
                }
            }
        } catch (Throwable t) {
            errors.add("调用 selftestParse 失败: " + t);
        }

        // 三个只读入口:在桌面上**真的调**一遍,拿"native 实现到底报了什么版本"。
        // 空壳库只认 Java_..._<name> 的符号名,返回值由生成的那份 C 决定
        // (见 check_kotlin_abi.ensure_stub_library)。
        // 注意:它们是 **实例方法**(Kotlin object 里的 external fun 不能标 @JvmStatic),
        // 所以必须拿一个实例来 invoke;invoke(null) 会在反射层就 NPE。
        // 这不是在测 SDK,是在测 **UI 的版本闸门**:库说 A、UI 要求 B,
        // 闸门就得以 A vs B 的形式说清楚,而不是后面莫名其妙崩。
        Object rendererInstance = null;
        for (String name : new String[]{"nativeAbiVersion", "nativeModelListProtocolVersion"}) {
            try {
                if (rendererInstance == null) {
                    java.lang.reflect.Field inst = renderer.getDeclaredField("INSTANCE");
                    inst.setAccessible(true);
                    rendererInstance = inst.get(null);
                }
                java.lang.reflect.Method m = renderer.getMethod(name);
                Object r = m.invoke(rendererInstance);
                values.put(name, String.valueOf(r));
            } catch (Throwable t) {
                Throwable cause = (t.getCause() != null) ? t.getCause() : t;
                // 记下来而不是抛出去:桌面校验要知道"调不通"这件事本身。
                values.put(name, "unavailable\t" + cause.getClass().getName()
                        + ": " + cause.getMessage());
            }
        }

        // 输出显式钉成 UTF-8 字节:-Dfile.encoding 在 JDK 18+ 之前是被 JEP 400 改过默认值的,
        // 而 Windows 上还可能是 GBK(中文模型名会变成 0xD0 这类字节,校验器一律解不出来)。
        // 协议语料里有中文,所以这里不能依赖平台默认编码。
        byte[] stdout = Json.write(out).getBytes("UTF-8");
        System.out.write(stdout, 0, stdout.length);
        System.out.write('\n');
        System.out.flush();
        // MbRenderer.<clinit> 里若真的 loadLibrary 成功,会留下 non-daemon 线程;
        // 显式退出,免得脚本挂在 JVM 上。
        Runtime.getRuntime().halt(0);
    }

    private static Object staticField(String binary, String field) {
        try {
            Class<?> c = Class.forName(binary, true, Probe.class.getClassLoader());
            java.lang.reflect.Field f = c.getDeclaredField(field);
            f.setAccessible(true);
            Object v = f.get(null);
            return (v instanceof Boolean) ? ((Boolean) v ? "true" : "false") : v;
        } catch (Throwable t) {
            return null;
        }
    }

    private static Map<String, Object> methodTable(Class<?> c) {
        Map<String, Object> table = new LinkedHashMap<>();
        java.lang.reflect.Method[] ms = c.getDeclaredMethods();
        Arrays.sort(ms, Comparator.comparing(java.lang.reflect.Method::getName)
                .thenComparing(Probe::sig));
        for (java.lang.reflect.Method m : ms) {
            Map<String, Object> e = new LinkedHashMap<>();
            e.put("ret", m.getReturnType().getName());
            List<String> ps = new ArrayList<>();
            for (Class<?> p : m.getParameterTypes()) ps.add(p.getName());
            e.put("params", ps);
            e.put("static", java.lang.reflect.Modifier.isStatic(m.getModifiers()));
            e.put("sig", sig(m));
            // 同名重载会互相覆盖,所以键里带上描述符;只关心 native* 的那几个名字。
            table.put(m.getName(), e);
        }
        return table;
    }

    private static String sig(java.lang.reflect.Method m) {
        StringBuilder sb = new StringBuilder("(");
        for (Class<?> p : m.getParameterTypes()) sb.append(d(p));
        return sb.append(")").append(d(m.getReturnType())).toString();
    }

    /** 极简 JSON:够读一个对象/数组/字符串/数字,不做通用实现。 */
    static final class Json {
        static Map<String, Object> parseObject(String s) {
            int[] i = {0};
            Object v = parse(s, i);
            @SuppressWarnings("unchecked")
            Map<String, Object> m = (Map<String, Object>) v;
            return m;
        }

        static Object parse(String s, int[] i) {
            skip(s, i);
            char c = s.charAt(i[0]);
            if (c == '{') {
                i[0]++;
                Map<String, Object> m = new LinkedHashMap<>();
                skip(s, i);
                if (s.charAt(i[0]) == '}') { i[0]++; return m; }
                while (true) {
                    skip(s, i);
                    String k = (String) parse(s, i);
                    skip(s, i);
                    i[0]++;  // ':'
                    m.put(k, parse(s, i));
                    skip(s, i);
                    char n = s.charAt(i[0]++);
                    if (n == '}') return m;
                }
            }
            if (c == '[') {
                i[0]++;
                List<Object> l = new ArrayList<>();
                skip(s, i);
                if (s.charAt(i[0]) == ']') { i[0]++; return l; }
                while (true) {
                    l.add(parse(s, i));
                    skip(s, i);
                    char n = s.charAt(i[0]++);
                    if (n == ']') return l;
                }
            }
            if (c == '"') {
                i[0]++;
                StringBuilder sb = new StringBuilder();
                while (true) {
                    char ch = s.charAt(i[0]++);
                    if (ch == '"') return sb.toString();
                    if (ch != '\\') { sb.append(ch); continue; }
                    char e = s.charAt(i[0]++);
                    switch (e) {
                        case 'n': sb.append('\n'); break;
                        case 't': sb.append('\t'); break;
                        case 'r': sb.append('\r'); break;
                        case 'b': sb.append('\b'); break;
                        case 'f': sb.append('\f'); break;
                        case 'u':
                            sb.append((char) Integer.parseInt(s.substring(i[0], i[0] + 4), 16));
                            i[0] += 4;
                            break;
                        default: sb.append(e);
                    }
                }
            }
            int start = i[0];
            while (i[0] < s.length() && "+-0123456789.eE".indexOf(s.charAt(i[0])) >= 0) i[0]++;
            String num = s.substring(start, i[0]);
            if (num.isEmpty()) return null;
            if (num.contains(".") || num.contains("e") || num.contains("E")) {
                return Double.parseDouble(num);
            }
            return Long.parseLong(num);
        }

        static void skip(String s, int[] i) {
            while (i[0] < s.length() && Character.isWhitespace(s.charAt(i[0]))) i[0]++;
        }

        static String str(String v) {
            StringBuilder sb = new StringBuilder("\"");
            for (char c : v.toCharArray()) {
                switch (c) {
                    case '"': sb.append("\\\""); break;
                    case '\\': sb.append("\\\\"); break;
                    case '\n': sb.append("\\n"); break;
                    case '\r': sb.append("\\r"); break;
                    case '\t': sb.append("\\t"); break;
                    default:
                        if (c < 0x20) sb.append(String.format("\\u%04x", (int) c));
                        else sb.append(c);
                }
            }
            return sb.append("\"").toString();
        }

        @SuppressWarnings("unchecked")
        static String write(Object v) {
            if (v == null) return "null";
            if (v instanceof String) return str((String) v);
            if (v instanceof Number || v instanceof Boolean) return String.valueOf(v);
            if (v instanceof Map) {
                StringBuilder sb = new StringBuilder("{");
                boolean first = true;
                for (Map.Entry<String, Object> e : ((Map<String, Object>) v).entrySet()) {
                    if (!first) sb.append(',');
                    first = false;
                    sb.append(str(e.getKey())).append(':').append(write(e.getValue()));
                }
                return sb.append('}').toString();
            }
            if (v instanceof List) {
                StringBuilder sb = new StringBuilder("[");
                boolean first = true;
                for (Object o : (List<Object>) v) {
                    if (!first) sb.append(',');
                    first = false;
                    sb.append(write(o));
                }
                return sb.append(']').toString();
            }
            return str(String.valueOf(v));
        }
    }
}
