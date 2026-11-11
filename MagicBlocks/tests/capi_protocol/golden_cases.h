// 由 tools/capi_protocol/gen_abi_table.py 从 tools/capi_protocol/protocol.json 生成。
// **不要手改** —— 改协议就改 protocol.json 再跑生成器,否则 --check 会红。
//
// 这份头文件是 C++ 与 JVM **共用的同一批字节**:
//   * C++ 侧(mb_capi_protocol_wire.cpp)拿它断言真实产出一致;
//   * 桌面程序(tests/capi_modellist/)再把同一批字节喂给**真实 Kotlin 编译产物**,
//     用 JVM 反射调用 MbRenderer.kt 里的解析函数。
// 两侧吃的是同一批文本,所以协议漂移不可能只漂一边。
#ifndef MAGICBLOCKS_TESTS_CAPI_PROTOCOL_GOLDEN_CASES_H
#define MAGICBLOCKS_TESTS_CAPI_PROTOCOL_GOLDEN_CASES_H

namespace mb_protocol_golden {

// ---- 协议常量(与 protocol.json / MbAbiProtocol.kt 同源) ----
#define MB_PROTO_FIELD_SEP "\t"
#define MB_PROTO_FIELD_SEP_CH '\t'
#define MB_PROTO_LINE_SEP_STR "\n"
#define MB_PROTO_LINE_SEP_CH '\n'
#define MB_PROTO_STATUS_PREFIX "err="
#define MB_PROTO_ERR_OK "MB_OK"
#define MB_PROTO_ERR_PREFIX_LEN 4
#define MB_PROTO_TOKEN_COUNT 5
#define MB_PROTO_PROTOCOL_VERSION 1

// 字段里被替换成空格的那几个字符(见 protocol.json 的 newline_sanitized)。
// 逐个字符给一条常量,而不是一整串:后者在生成物里换行后会变成非法字面量。
#define MB_PROTO_SANITIZE_CHAR_0 '\x0D'  // 
#define MB_PROTO_SANITIZE_CHAR_1 '\x0A'  // \n
#define MB_PROTO_SANITIZE_CHAR_2 '\x09'  // \t
#define MB_PROTO_SANITIZE_CHAR_COUNT 3
#define MB_PROTO_SANITIZE_CHAR ' '

// ---- 字段顺序(索引即协议位置) ----
#define MB_PROTO_FIELD_0_NAME 0
#define MB_PROTO_FIELD_1_PATH 1
#define MB_PROTO_FIELD_2_BYTES 2
#define MB_PROTO_FIELD_3_OK 3
#define MB_PROTO_FIELD_4_MESSAGE 4

struct case_t {
  const char* key;
  int kind;  // 0 = ok, 1 = err
  const char* text;  // 完整返回文本(含末尾换行)
};

constexpr case_t kCases[] = {
    {"empty_dir", 0, "err=MB_OK\n"},
    {"one_model", 0, "err=MB_OK\ncube\n/data/user/0/com.magicblocks.pad/cache/models/cube.obj\n717\n1\n\n面板\n/data/user/0/com.magicblocks.pad/cache/models/panel.obj\n350\n0\nOBJ 里没有三角形\n"},
    {"not_found", 1, "err=MB_E_NOT_FOUND\n"},
    {"io_error", 1, "err=MB_E_IO\n"},
};

constexpr int kCaseCount = 4;

}  // namespace mb_protocol_golden

#endif  // MAGICBLOCKS_TESTS_CAPI_PROTOCOL_GOLDEN_CASES_H
