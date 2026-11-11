// MagicBlocks SDK —— 统一错误返回
//
// spec D8「失败即报错」:能力/预算不足 → 明确报错,不静默降级。
// 所有会失败的 SDK 入口都返回 Result<T>,错误同时携带机器可判的 code 与
// 人类可读的 message —— 上层据此报错,绝不臆测「差不多能跑」。
#pragma once

#include <string>
#include <utility>

namespace magicblocks::device {

enum class StatusCode : std::uint8_t {
  Ok = 0,
  InvalidArgument,       // 参数非法(如 in_flight_frames 不在 {2,3})
  Unsupported,           // 能力不足/后端未实现/平台不支持
  OutOfMemory,           // 预算不足
  DeviceLost,
  InitializationFailed,
  ShaderCompileFailed,
  IoError,               // 文件读写失败
};

struct Status {
  StatusCode code = StatusCode::Ok;
  std::string message;

  Status() = default;
  Status(StatusCode c, std::string msg) : code(c), message(std::move(msg)) {}

  bool ok() const { return code == StatusCode::Ok; }
  explicit operator bool() const { return ok(); }

  static Status OkStatus() { return {}; }
  static Status Ok_() { return {}; }
  static Status Invalid(std::string m) { return {StatusCode::InvalidArgument, std::move(m)}; }
  static Status Unsupported(std::string m) { return {StatusCode::Unsupported, std::move(m)}; }
  static Status OOM(std::string m) { return {StatusCode::OutOfMemory, std::move(m)}; }
  static Status InitFailed(std::string m) { return {StatusCode::InitializationFailed, std::move(m)}; }
  static Status DeviceLost(std::string m) { return {StatusCode::DeviceLost, std::move(m)}; }
  static Status Io(std::string m) { return {StatusCode::IoError, std::move(m)}; }
};

// Result<T>:要么是有效值,要么是错误。错误必须被检查(取 value 前先看 ok())。
template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)), status_() {}         // NOLINT(*-explicit-*)
  Result(Status st) : status_(std::move(st)) {}                    // NOLINT(*-explicit-*)

  bool ok() const { return status_.ok(); }
  explicit operator bool() const { return ok(); }

  const Status& status() const { return status_; }
  const std::string& message() const { return status_.message; }

  // 仅在 ok() 时调用。
  T& value() { return value_; }
  const T& value() const { return value_; }
  T&& take() { return std::move(value_); }

 private:
  T value_{};
  Status status_{};
};

}  // namespace magicblocks::device
