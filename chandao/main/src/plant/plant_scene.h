#pragma once

#include "seed.h"

#include <QVector>
#include <cstdint>

// 与 Vulkan 顶点输入一致：NDC [-1,1]，Y 向上。
struct PlantVertex {
    float x = 0.f;
    float y = 0.f;
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    float a = 1.f;
};

struct PlantPhase {
    int index = 0;       // 0..visualCount-1，对应用户阶段 1..N+1
    double localT = 0.0; // [0,1)
    int visualCount = 6;
};

namespace PlantScene {

PlantPhase phaseOf(const SeedDescriptor &seed, double progress);
QVector<PlantVertex> build(const SeedDescriptor &seed, double progress);

} // namespace PlantScene
