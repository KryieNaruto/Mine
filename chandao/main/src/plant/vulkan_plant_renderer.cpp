#ifdef HAS_VULKAN

#include "vulkan_plant_renderer.h"

#include <QFile>
#include <QImage>
#include <cstring>
#include <vector>
#include <cstddef>

#include <vulkan/vulkan.h>

namespace {

constexpr uint32_t kMaxVertices = 4096;

QString vkErr(VkResult r, const char *what)
{
    return QStringLiteral("%1 (VkResult %2)").arg(QLatin1String(what)).arg(int(r));
}

} // namespace

struct VulkanPlantRenderer::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t graphicsFamily = 0;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    void *vertexMapped = nullptr;

    int offscreenSize = 0;
    VkImage colorImage = VK_NULL_HANDLE;
    VkDeviceMemory colorMemory = VK_NULL_HANDLE;
    VkImageView colorView = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void *stagingMapped = nullptr;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};

VulkanPlantRenderer::~VulkanPlantRenderer()
{
    destroyDeviceObjects();
    delete m_impl;
    m_impl = nullptr;
    m_ready = false;
}

bool VulkanPlantRenderer::init()
{
    if (m_ready) {
        return true;
    }
    m_error.clear();
    if (!m_impl) {
        m_impl = new Impl();
    }
    if (!createInstance() || !pickDevice() || !createDevice()
        || !createCommandPool() || !createRenderPass() || !createPipeline()
        || !createVertexBuffer()) {
        destroyDeviceObjects();
        return false;
    }
    m_ready = true;
    return true;
}

void VulkanPlantRenderer::destroyOffscreen()
{
    if (!m_impl || !m_impl->device) {
        return;
    }
    vkDeviceWaitIdle(m_impl->device);
    if (m_impl->cmd) {
        vkFreeCommandBuffers(m_impl->device, m_impl->cmdPool, 1, &m_impl->cmd);
        m_impl->cmd = VK_NULL_HANDLE;
    }
    if (m_impl->framebuffer) {
        vkDestroyFramebuffer(m_impl->device, m_impl->framebuffer, nullptr);
        m_impl->framebuffer = VK_NULL_HANDLE;
    }
    if (m_impl->colorView) {
        vkDestroyImageView(m_impl->device, m_impl->colorView, nullptr);
        m_impl->colorView = VK_NULL_HANDLE;
    }
    if (m_impl->colorImage) {
        vkDestroyImage(m_impl->device, m_impl->colorImage, nullptr);
        m_impl->colorImage = VK_NULL_HANDLE;
    }
    if (m_impl->colorMemory) {
        vkFreeMemory(m_impl->device, m_impl->colorMemory, nullptr);
        m_impl->colorMemory = VK_NULL_HANDLE;
    }
    if (m_impl->stagingMapped) {
        vkUnmapMemory(m_impl->device, m_impl->stagingMemory);
        m_impl->stagingMapped = nullptr;
    }
    if (m_impl->stagingBuffer) {
        vkDestroyBuffer(m_impl->device, m_impl->stagingBuffer, nullptr);
        m_impl->stagingBuffer = VK_NULL_HANDLE;
    }
    if (m_impl->stagingMemory) {
        vkFreeMemory(m_impl->device, m_impl->stagingMemory, nullptr);
        m_impl->stagingMemory = VK_NULL_HANDLE;
    }
    m_impl->offscreenSize = 0;
}

void VulkanPlantRenderer::destroyDeviceObjects()
{
    if (!m_impl) {
        return;
    }
    destroyOffscreen();
    if (m_impl->device) {
        vkDeviceWaitIdle(m_impl->device);
        if (m_impl->vertexMapped) {
            vkUnmapMemory(m_impl->device, m_impl->vertexMemory);
            m_impl->vertexMapped = nullptr;
        }
        if (m_impl->vertexBuffer) {
            vkDestroyBuffer(m_impl->device, m_impl->vertexBuffer, nullptr);
            m_impl->vertexBuffer = VK_NULL_HANDLE;
        }
        if (m_impl->vertexMemory) {
            vkFreeMemory(m_impl->device, m_impl->vertexMemory, nullptr);
            m_impl->vertexMemory = VK_NULL_HANDLE;
        }
        if (m_impl->pipeline) {
            vkDestroyPipeline(m_impl->device, m_impl->pipeline, nullptr);
            m_impl->pipeline = VK_NULL_HANDLE;
        }
        if (m_impl->pipelineLayout) {
            vkDestroyPipelineLayout(m_impl->device, m_impl->pipelineLayout, nullptr);
            m_impl->pipelineLayout = VK_NULL_HANDLE;
        }
        if (m_impl->vertModule) {
            vkDestroyShaderModule(m_impl->device, m_impl->vertModule, nullptr);
            m_impl->vertModule = VK_NULL_HANDLE;
        }
        if (m_impl->fragModule) {
            vkDestroyShaderModule(m_impl->device, m_impl->fragModule, nullptr);
            m_impl->fragModule = VK_NULL_HANDLE;
        }
        if (m_impl->renderPass) {
            vkDestroyRenderPass(m_impl->device, m_impl->renderPass, nullptr);
            m_impl->renderPass = VK_NULL_HANDLE;
        }
        if (m_impl->cmdPool) {
            vkDestroyCommandPool(m_impl->device, m_impl->cmdPool, nullptr);
            m_impl->cmdPool = VK_NULL_HANDLE;
        }
        vkDestroyDevice(m_impl->device, nullptr);
        m_impl->device = VK_NULL_HANDLE;
    }
    if (m_impl->instance) {
        vkDestroyInstance(m_impl->instance, nullptr);
        m_impl->instance = VK_NULL_HANDLE;
    }
    m_ready = false;
}

uint32_t VulkanPlantRenderer::findMemoryType(uint32_t typeFilter, uint32_t properties) const
{
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(m_impl->physical, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i))
            && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VulkanPlantRenderer::createInstance()
{
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "chandao-plant";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "chandao";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    const VkResult r = vkCreateInstance(&ci, nullptr, &m_impl->instance);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkCreateInstance");
        return false;
    }
    return true;
}

bool VulkanPlantRenderer::pickDevice()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_impl->instance, &count, nullptr);
    if (count == 0) {
        m_error = QStringLiteral("no Vulkan physical device");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_impl->instance, &count, devices.data());

    for (VkPhysicalDevice pd : devices) {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, qs.data());
        for (uint32_t i = 0; i < qCount; ++i) {
            if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                m_impl->physical = pd;
                m_impl->graphicsFamily = i;
                return true;
            }
        }
    }
    m_error = QStringLiteral("no graphics queue family");
    return false;
}

bool VulkanPlantRenderer::createDevice()
{
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = m_impl->graphicsFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    const VkResult r = vkCreateDevice(m_impl->physical, &ci, nullptr, &m_impl->device);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkCreateDevice");
        return false;
    }
    vkGetDeviceQueue(m_impl->device, m_impl->graphicsFamily, 0, &m_impl->queue);
    return true;
}

bool VulkanPlantRenderer::createCommandPool()
{
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = m_impl->graphicsFamily;
    const VkResult r = vkCreateCommandPool(m_impl->device, &ci, nullptr, &m_impl->cmdPool);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkCreateCommandPool");
        return false;
    }
    return true;
}

bool VulkanPlantRenderer::createRenderPass()
{
    VkAttachmentDescription color{};
    color.format = VK_FORMAT_B8G8R8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &color;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    const VkResult r = vkCreateRenderPass(m_impl->device, &ci, nullptr, &m_impl->renderPass);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkCreateRenderPass");
        return false;
    }
    return true;
}

bool VulkanPlantRenderer::loadShader(const char *qrcPath, void **outModule)
{
    QFile f(QString::fromUtf8(qrcPath));
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("cannot open shader %1").arg(QLatin1String(qrcPath));
        return false;
    }
    const QByteArray data = f.readAll();
    if (data.size() < 4 || (data.size() % 4) != 0) {
        m_error = QStringLiteral("invalid SPIR-V %1").arg(QLatin1String(qrcPath));
        return false;
    }
    std::vector<uint32_t> spirv(static_cast<size_t>(data.size() / 4));
    std::memcpy(spirv.data(), data.constData(), static_cast<size_t>(data.size()));
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = static_cast<size_t>(data.size());
    ci.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult r = vkCreateShaderModule(m_impl->device, &ci, nullptr, &module);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkCreateShaderModule");
        return false;
    }
    *outModule = module;
    return true;
}

bool VulkanPlantRenderer::createPipeline()
{
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    void *vPtr = nullptr;
    void *fPtr = nullptr;
    if (!loadShader(":/shaders/plant.vert.spv", &vPtr)
        || !loadShader(":/shaders/plant.frag.spv", &fPtr)) {
        return false;
    }
    vert = static_cast<VkShaderModule>(vPtr);
    frag = static_cast<VkShaderModule>(fPtr);
    m_impl->vertModule = vert;
    m_impl->fragModule = frag;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = sizeof(PlantVertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = offsetof(PlantVertex, x);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[1].offset = offsetof(PlantVertex, r);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.blendEnable = VK_TRUE;
    blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.colorBlendOp = VK_BLEND_OP_ADD;
    blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blendAtt;

    VkDynamicState dynStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkPipelineLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkResult r = vkCreatePipelineLayout(m_impl->device, &lci, nullptr, &m_impl->pipelineLayout);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkCreatePipelineLayout");
        return false;
    }

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = m_impl->pipelineLayout;
    pci.renderPass = m_impl->renderPass;
    pci.subpass = 0;
    r = vkCreateGraphicsPipelines(m_impl->device, VK_NULL_HANDLE, 1, &pci, nullptr,
                                  &m_impl->pipeline);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkCreateGraphicsPipelines");
        return false;
    }
    return true;
}

bool VulkanPlantRenderer::createVertexBuffer()
{
    const VkDeviceSize size = sizeof(PlantVertex) * kMaxVertices;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(m_impl->device, &bi, nullptr, &m_impl->vertexBuffer);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkCreateBuffer(vertex)");
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_impl->device, m_impl->vertexBuffer, &req);
    const uint32_t type = findMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        m_error = QStringLiteral("no host-visible memory for vertex buffer");
        return false;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    r = vkAllocateMemory(m_impl->device, &ai, nullptr, &m_impl->vertexMemory);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkAllocateMemory(vertex)");
        return false;
    }
    vkBindBufferMemory(m_impl->device, m_impl->vertexBuffer, m_impl->vertexMemory, 0);
    r = vkMapMemory(m_impl->device, m_impl->vertexMemory, 0, size, 0, &m_impl->vertexMapped);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkMapMemory(vertex)");
        return false;
    }
    return true;
}

bool VulkanPlantRenderer::ensureOffscreen(int size)
{
    if (size < 1) {
        m_error = QStringLiteral("invalid offscreen size");
        return false;
    }
    if (m_impl->offscreenSize == size && m_impl->framebuffer) {
        return true;
    }
    destroyOffscreen();

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = { uint32_t(size), uint32_t(size), 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(m_impl->device, &ii, nullptr, &m_impl->colorImage);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkCreateImage");
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_impl->device, m_impl->colorImage, &req);
    const uint32_t type = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        m_error = QStringLiteral("no DEVICE_LOCAL memory for color attachment");
        return false;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    r = vkAllocateMemory(m_impl->device, &ai, nullptr, &m_impl->colorMemory);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkAllocateMemory(image)");
        return false;
    }
    vkBindImageMemory(m_impl->device, m_impl->colorImage, m_impl->colorMemory, 0);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_impl->colorImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_B8G8R8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    r = vkCreateImageView(m_impl->device, &vi, nullptr, &m_impl->colorView);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkCreateImageView");
        return false;
    }

    VkFramebufferCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = m_impl->renderPass;
    fi.attachmentCount = 1;
    fi.pAttachments = &m_impl->colorView;
    fi.width = uint32_t(size);
    fi.height = uint32_t(size);
    fi.layers = 1;
    r = vkCreateFramebuffer(m_impl->device, &fi, nullptr, &m_impl->framebuffer);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkCreateFramebuffer");
        return false;
    }

    const VkDeviceSize stagingSize = VkDeviceSize(size) * VkDeviceSize(size) * 4;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = stagingSize;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(m_impl->device, &bi, nullptr, &m_impl->stagingBuffer);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkCreateBuffer(staging)");
        return false;
    }
    VkMemoryRequirements sreq{};
    vkGetBufferMemoryRequirements(m_impl->device, m_impl->stagingBuffer, &sreq);
    const uint32_t stype = findMemoryType(
        sreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (stype == UINT32_MAX) {
        m_error = QStringLiteral("no host-visible memory for staging");
        return false;
    }
    VkMemoryAllocateInfo sai{};
    sai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    sai.allocationSize = sreq.size;
    sai.memoryTypeIndex = stype;
    r = vkAllocateMemory(m_impl->device, &sai, nullptr, &m_impl->stagingMemory);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkAllocateMemory(staging)");
        return false;
    }
    vkBindBufferMemory(m_impl->device, m_impl->stagingBuffer, m_impl->stagingMemory, 0);
    r = vkMapMemory(m_impl->device, m_impl->stagingMemory, 0, stagingSize, 0,
                    &m_impl->stagingMapped);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkMapMemory(staging)");
        return false;
    }

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = m_impl->cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    r = vkAllocateCommandBuffers(m_impl->device, &cai, &m_impl->cmd);
    if (r != VK_SUCCESS) {
        m_error = vkErr(r, "vkAllocateCommandBuffers");
        return false;
    }

    m_impl->offscreenSize = size;
    return true;
}

QImage VulkanPlantRenderer::render(const QVector<PlantVertex> &verts, int physicalSize)
{
    if (!m_ready && !init()) {
        return {};
    }
    physicalSize = qBound(1, physicalSize, 512);
    if (!ensureOffscreen(physicalSize)) {
        return {};
    }

    uint32_t vertCount = uint32_t(qMin(verts.size(), int(kMaxVertices)));
    vertCount -= vertCount % 3;
    if (m_impl->vertexMapped && vertCount > 0) {
        std::memcpy(m_impl->vertexMapped, verts.constData(),
                    sizeof(PlantVertex) * vertCount);
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(m_impl->cmd, 0);
    vkBeginCommandBuffer(m_impl->cmd, &bi);

    VkClearValue clear{};
    clear.color = { { 0.f, 0.f, 0.f, 0.f } };
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_impl->renderPass;
    rp.framebuffer = m_impl->framebuffer;
    rp.renderArea.extent = { uint32_t(physicalSize), uint32_t(physicalSize) };
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(m_impl->cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = float(physicalSize);
    viewport.height = float(physicalSize);
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(m_impl->cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = { uint32_t(physicalSize), uint32_t(physicalSize) };
    vkCmdSetScissor(m_impl->cmd, 0, 1, &scissor);

    vkCmdBindPipeline(m_impl->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_impl->pipeline);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(m_impl->cmd, 0, 1, &m_impl->vertexBuffer, &offset);
    if (vertCount > 0) {
        vkCmdDraw(m_impl->cmd, vertCount, 1, 0, 0);
    }
    vkCmdEndRenderPass(m_impl->cmd);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_impl->colorImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(m_impl->cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = { uint32_t(physicalSize), uint32_t(physicalSize), 1 };
    vkCmdCopyImageToBuffer(m_impl->cmd, m_impl->colorImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_impl->stagingBuffer, 1, &copy);

    vkEndCommandBuffer(m_impl->cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_impl->cmd;
    const VkResult sr = vkQueueSubmit(m_impl->queue, 1, &submit, VK_NULL_HANDLE);
    if (sr != VK_SUCCESS) {
        m_error = vkErr(sr, "vkQueueSubmit");
        return {};
    }
    vkQueueWaitIdle(m_impl->queue);

    QImage image(physicalSize, physicalSize, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull() || !m_impl->stagingMapped) {
        m_error = QStringLiteral("staging readback failed");
        return {};
    }
    const int srcStride = physicalSize * 4;
    const auto *src = static_cast<const uchar *>(m_impl->stagingMapped);
    for (int y = 0; y < physicalSize; ++y) {
        // Vulkan 原点在左下，Qt 在左上：逐行翻转
        std::memcpy(image.scanLine(physicalSize - 1 - y), src + y * srcStride,
                    size_t(srcStride));
    }
    // 预乘：片元直通 alpha，透明像素 rgb 已为 0
    for (int y = 0; y < physicalSize; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < physicalSize; ++x) {
            const QRgb px = line[x];
            const int a = qAlpha(px);
            if (a == 0 || a == 255) {
                continue;
            }
            line[x] = qRgba(qRed(px) * a / 255, qGreen(px) * a / 255,
                            qBlue(px) * a / 255, a);
        }
    }
    return image;
}

#endif // HAS_VULKAN
