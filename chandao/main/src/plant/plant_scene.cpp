#include "plant_scene.h"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

struct Rgba {
    float r, g, b, a;
};

constexpr Rgba kPot      = { 0xB8 / 255.f, 0x6B / 255.f, 0x45 / 255.f, 1.f };
constexpr Rgba kPotRim   = { 0xC8 / 255.f, 0x7A / 255.f, 0x52 / 255.f, 1.f };
constexpr Rgba kSoil     = { 0x4A / 255.f, 0x3B / 255.f, 0x32 / 255.f, 1.f };
constexpr Rgba kSprout   = { 0x8B / 255.f, 0xCF / 255.f, 0x6A / 255.f, 1.f };
constexpr Rgba kLeaf     = { 0x5A / 255.f, 0xAE / 255.f, 0x5F / 255.f, 1.f };
constexpr Rgba kStemLight= { 0xA8 / 255.f, 0xC9 / 255.f, 0x7A / 255.f, 1.f };
constexpr Rgba kStemDark = { 0x6B / 255.f, 0x4F / 255.f, 0x2A / 255.f, 1.f };
constexpr Rgba kTrunk    = { 0x3E / 255.f, 0x2A / 255.f, 0x1A / 255.f, 1.f };

Rgba lerp(const Rgba &a, const Rgba &b, float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return {
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t,
    };
}

void addTri(QVector<PlantVertex> &out,
            float x0, float y0, float x1, float y1, float x2, float y2,
            const Rgba &c)
{
    out.push_back({ x0, y0, c.r, c.g, c.b, c.a });
    out.push_back({ x1, y1, c.r, c.g, c.b, c.a });
    out.push_back({ x2, y2, c.r, c.g, c.b, c.a });
}

void addQuad(QVector<PlantVertex> &out,
             float x0, float y0, float x1, float y1,
             float x2, float y2, float x3, float y3,
             const Rgba &c)
{
    addTri(out, x0, y0, x1, y1, x2, y2, c);
    addTri(out, x0, y0, x2, y2, x3, y3, c);
}

void addEllipse(QVector<PlantVertex> &out,
                float cx, float cy, float rx, float ry,
                const Rgba &c, int segs = 20,
                float rot = 0.f)
{
    const float cr = std::cos(rot);
    const float sr = std::sin(rot);
    auto pt = [&](int i) {
        const float a = float(i) / float(segs) * float(2.0 * M_PI);
        const float lx = std::cos(a) * rx;
        const float ly = std::sin(a) * ry;
        return std::pair<float, float>{ cx + lx * cr - ly * sr, cy + lx * sr + ly * cr };
    };
    auto [xPrev, yPrev] = pt(0);
    for (int i = 1; i <= segs; ++i) {
        auto [x, y] = pt(i);
        addTri(out, cx, cy, xPrev, yPrev, x, y, c);
        xPrev = x;
        yPrev = y;
    }
}

void addTrapezoid(QVector<PlantVertex> &out,
                  float yBottom, float yTop,
                  float halfBottom, float halfTop,
                  const Rgba &c)
{
    addQuad(out,
            -halfBottom, yBottom, halfBottom, yBottom,
            halfTop, yTop, -halfTop, yTop,
            c);
}

void addStem(QVector<PlantVertex> &out,
             float x, float y0, float y1, float halfW, const Rgba &c)
{
    addQuad(out,
            x - halfW, y0, x + halfW, y0,
            x + halfW * 0.85f, y1, x - halfW * 0.85f, y1,
            c);
}

void addPotAndSoil(QVector<PlantVertex> &out)
{
    addTrapezoid(out, -0.82f, -0.52f, 0.22f, 0.30f, kPot);
    addQuad(out, -0.32f, -0.52f, 0.32f, -0.52f, 0.30f, -0.46f, -0.30f, -0.46f, kPotRim);
    addEllipse(out, 0.f, -0.50f, 0.26f, 0.075f, kSoil, 22);
}

void addSprout(QVector<PlantVertex> &out, float height, float halfW)
{
    addStem(out, 0.f, -0.48f, -0.48f + height, halfW, kSprout);
    addEllipse(out, 0.f, -0.48f + height, halfW * 1.6f, halfW * 1.2f, kSprout, 14);
}

void addTwoLeaves(QVector<PlantVertex> &out, float stemTop)
{
    addEllipse(out, -0.12f, stemTop - 0.02f, 0.13f, 0.06f, kLeaf, 18, 0.45f);
    addEllipse(out,  0.12f, stemTop - 0.02f, 0.13f, 0.06f, kLeaf, 18, -0.45f);
}

void addSideLeaves(QVector<PlantVertex> &out, float y0, float y1, int count)
{
    if (count <= 0) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        const float t = (count == 1) ? 0.5f : float(i) / float(count - 1);
        const float y = y0 + (y1 - y0) * t;
        const float side = (i % 2 == 0) ? -1.f : 1.f;
        const float rx = 0.09f + 0.03f * t;
        addEllipse(out, side * (0.08f + 0.04f * t), y, rx, 0.045f, kLeaf, 16,
                   side * 0.5f);
    }
}

void addCanopy(QVector<PlantVertex> &out, float topY, float scale)
{
    const Rgba dark = kLeaf;
    const Rgba light = kSprout;
    struct Blob { float dx, dy, rx, ry; };
    const Blob blobs[] = {
        {  0.00f,  0.00f, 0.34f, 0.26f },
        { -0.22f, -0.04f, 0.22f, 0.18f },
        {  0.22f, -0.04f, 0.22f, 0.18f },
        { -0.12f,  0.12f, 0.20f, 0.16f },
        {  0.12f,  0.12f, 0.20f, 0.16f },
        {  0.00f,  0.18f, 0.18f, 0.14f },
        { -0.28f,  0.06f, 0.16f, 0.13f },
        {  0.28f,  0.06f, 0.16f, 0.13f },
        { -0.18f, -0.14f, 0.15f, 0.12f },
        {  0.18f, -0.14f, 0.15f, 0.12f },
        {  0.00f, -0.16f, 0.20f, 0.12f },
        { -0.08f,  0.22f, 0.12f, 0.10f },
        {  0.08f,  0.22f, 0.12f, 0.10f },
    };
    int i = 0;
    for (const Blob &b : blobs) {
        const Rgba c = (i++ % 2 == 0) ? dark : light;
        addEllipse(out, b.dx * scale, topY + b.dy * scale,
                   b.rx * scale, b.ry * scale, c, 18);
    }
}

void addTrunk(QVector<PlantVertex> &out, float y0, float y1, float halfW, const Rgba &c)
{
    addStem(out, 0.f, y0, y1, halfW, c);
    // 两侧短枝节，类似花园树身的起伏
    addQuad(out,
            -halfW * 1.15f, y0 + (y1 - y0) * 0.35f,
            -halfW * 0.20f, y0 + (y1 - y0) * 0.32f,
            -halfW * 0.20f, y0 + (y1 - y0) * 0.48f,
            -halfW * 1.35f, y0 + (y1 - y0) * 0.45f,
            c);
    addQuad(out,
            halfW * 0.20f, y0 + (y1 - y0) * 0.55f,
            halfW * 1.25f, y0 + (y1 - y0) * 0.52f,
            halfW * 1.15f, y0 + (y1 - y0) * 0.66f,
            halfW * 0.20f, y0 + (y1 - y0) * 0.68f,
            c);
}

} // namespace

PlantPhase PlantScene::phaseOf(const SeedDescriptor &seed, double progress)
{
    const int visualCount = std::max(1, seed.timeStageCount + 1);
    const double p = std::clamp(progress, 0.0, 1.0);
    int index = static_cast<int>(std::floor(p * visualCount));
    if (index >= visualCount) {
        index = visualCount - 1;
    }
    const double localT = p * visualCount - index;
    return { index, std::clamp(localT, 0.0, 1.0), visualCount };
}

QVector<PlantVertex> PlantScene::build(const SeedDescriptor &seed, double progress)
{
    QVector<PlantVertex> out;
    out.reserve(2048);
    addPotAndSoil(out);

    const PlantPhase ph = phaseOf(seed, progress);
    switch (ph.index) {
    case 0: // 阶段1：只见盆土
        break;
    case 1: { // 阶段2：小芽
        addSprout(out, 0.18f, 0.028f);
        break;
    }
    case 2: { // 阶段3：抽叶
        addSprout(out, 0.28f, 0.030f);
        addTwoLeaves(out, -0.48f + 0.28f);
        break;
    }
    case 3: { // 阶段4：持续长高、出叶、茎变粗变深
        const float t = static_cast<float>(ph.localT);
        const float h = 0.28f + 0.28f * t;
        const float w = 0.032f + 0.040f * t;
        const Rgba stem = lerp(kStemLight, kStemDark, t);
        addStem(out, 0.f, -0.48f, -0.48f + h, w, stem);
        addTwoLeaves(out, -0.48f + h);
        addSideLeaves(out, -0.42f, -0.48f + h * 0.85f, 2 + static_cast<int>(t * 6.f));
        break;
    }
    case 4: { // 阶段5：树冠骤现，高度缓慢增加
        const float t = static_cast<float>(ph.localT);
        const float burst = std::min(1.f, t * 8.f);
        const float h = 0.52f + 0.10f * t;
        addStem(out, 0.f, -0.48f, -0.48f + h, 0.072f, kStemDark);
        addCanopy(out, -0.48f + h, 0.72f + 0.28f * burst);
        break;
    }
    case 5:
    default: { // 阶段6：只见树干
        addTrunk(out, -0.48f, 0.32f, 0.16f, kTrunk);
        break;
    }
    }
    return out;
}
