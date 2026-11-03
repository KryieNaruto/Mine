// MagicBlocks —— Android NDK 的 ABI 形状 stub(仅编译校验用,永不链接)
//
// 目的:让 vulkan_swapchain_android.cpp 在没有 NDK 的 Windows/Linux 机器上也能
// 做语法/类型校验。这里只声明**类型名**,不含任何实现;
// 有 NDK 时(MB_ANDROID_USE_NDK=1)本头不会被包含,改走真实
// <android/native_window.h>。
#pragma once

// ANativeWindow 在 NDK 中是 opaque struct;Vulkan 只把它当指针传递。
struct ANativeWindow;
