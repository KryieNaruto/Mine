#pragma once

#include "plant_scene.h"

#include <QImage>
#include <QString>
#include <QVector>

#ifdef HAS_VULKAN

// 离屏 Vulkan 渲染：DEVICE_LOCAL OPTIMAL 附件 + staging 回读。
class VulkanPlantRenderer
{
public:
    VulkanPlantRenderer() = default;
    ~VulkanPlantRenderer();

    VulkanPlantRenderer(const VulkanPlantRenderer &) = delete;
    VulkanPlantRenderer &operator=(const VulkanPlantRenderer &) = delete;

    bool init();
    bool isReady() const { return m_ready; }
    const QString &lastError() const { return m_error; }

    // physicalSize 为物理像素边长（已钳制 ≤512）。返回 ARGB32_Premultiplied。
    QImage render(const QVector<PlantVertex> &verts, int physicalSize);

private:
    bool createInstance();
    bool pickDevice();
    bool createDevice();
    bool createCommandPool();
    bool createRenderPass();
    bool createPipeline();
    bool createVertexBuffer();
    bool ensureOffscreen(int size);
    void destroyOffscreen();
    void destroyDeviceObjects();
    uint32_t findMemoryType(uint32_t typeFilter, uint32_t properties) const;
    bool loadShader(const char *qrcPath, void **outModule);

    bool m_ready = false;
    QString m_error;

    struct Impl;
    Impl *m_impl = nullptr;
};

#endif // HAS_VULKAN
