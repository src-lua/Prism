#include "Prism/vulkan/vulkan_renderer.hpp"

#include "Prism/core/style.hpp"

#include <fstream>

namespace Prism {

#define TYPE_SPHERE 0
#define TYPE_PLANE 1

VulkanRenderer::VulkanRenderer(VulkanContext& context, const SceneDataGPU& sceneData, uint32_t imageWidth, uint32_t imageHeight, std::string shaderPath)
    : m_context(context), m_imageWidth(imageWidth), m_imageHeight(imageHeight) {
    try {
        createBuffers(sceneData.getCamera(), sceneData.getObjects(), sceneData.getMaterials(), sceneData.getLights());
        createDescriptorSet();
        createComputePipeline(shaderPath);
        createCommandObjects();
        Style::logInfo("VulkanRenderer initialized.");
    } catch (const vk::Error& e) {
        Style::logError("VulkanRenderer initialization failed: " + std::string(e.what()));
        throw;
    }
}

VulkanRenderer::~VulkanRenderer() {
    m_context.getDevice().waitIdle();

    if (m_fence)
        m_context.getDevice().destroyFence(m_fence);
    if (m_commandPool)
        m_context.getDevice().destroyCommandPool(m_commandPool);
    if (m_computePipeline)
        m_context.getDevice().destroyPipeline(m_computePipeline);
    if (m_pipelineLayout)
        m_context.getDevice().destroyPipelineLayout(m_pipelineLayout);
    if (m_descriptorPool)
        m_context.getDevice().destroyDescriptorPool(m_descriptorPool);
    if (m_descriptorSetLayout)
        m_context.getDevice().destroyDescriptorSetLayout(m_descriptorSetLayout);
    if (m_lightBuffer)
        vmaDestroyBuffer(m_context.getAllocator(), m_lightBuffer, m_lightBufferAllocation);
    if (m_materialBuffer)
        vmaDestroyBuffer(m_context.getAllocator(), m_materialBuffer, m_materialBufferAllocation);
    if (m_objectBuffer)
        vmaDestroyBuffer(m_context.getAllocator(), m_objectBuffer, m_objectBufferAllocation);
    if (m_cameraBuffer)
        vmaDestroyBuffer(m_context.getAllocator(), m_cameraBuffer, m_cameraBufferAllocation);
    if (m_outputBuffer)
        vmaDestroyBuffer(m_context.getAllocator(), m_outputBuffer, m_outputBufferAllocation);

    Style::logInfo("VulkanRenderer destroyed.");
}

void VulkanRenderer::render() {
    // Espera e reseta o fence para garantir que a GPU não esteja ocupada
    (void)m_context.getDevice().waitForFences(1, &m_fence, VK_TRUE, UINT64_MAX);
    m_context.getDevice().resetFences(1, &m_fence);

    // Começa a gravar comandos
    m_commandBuffer.begin(
        vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

    // Associa o pipeline e os descritores
    m_commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_computePipeline);
    m_commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_pipelineLayout, 0, 1,
                                       &m_descriptorSet, 0, nullptr);

    // Dispara o shader! Divide o trabalho em grupos de 32x32.
    m_commandBuffer.dispatch((m_imageWidth + 31) / 32, (m_imageHeight + 31) / 32, 1);

    // Termina a gravação
    m_commandBuffer.end();

    // Submete os comandos para a fila de computação da GPU
    vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, &m_commandBuffer);
    m_context.getComputeQueue().submit(1, &submitInfo, m_fence);

    // Espera a GPU terminar o trabalho
    (void)m_context.getDevice().waitForFences(1, &m_fence, VK_TRUE, UINT64_MAX);

    Style::logDone("Render pass complete.");
}

void VulkanRenderer::saveImage(const std::string& filename) {
    // 1. Cria um buffer de "staging" que a CPU pode acessar
    vk::DeviceSize bufferSize = m_imageWidth * m_imageHeight * 4 * sizeof(float);
    vk::BufferCreateInfo stagingBufferInfo({}, bufferSize, vk::BufferUsageFlagBits::eTransferDst);

    VmaAllocationCreateInfo stagingAllocInfo = {};
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    vk::Buffer stagingBuffer;
    VmaAllocation stagingAllocation;
    vmaCreateBuffer(m_context.getAllocator(),
                    reinterpret_cast<const VkBufferCreateInfo*>(&stagingBufferInfo),
                    &stagingAllocInfo, reinterpret_cast<VkBuffer*>(&stagingBuffer),
                    &stagingAllocation, nullptr);

    // 2. Grava um comando para copiar do buffer da GPU para o buffer de staging
    m_commandBuffer.reset();
    m_commandBuffer.begin(
        vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    vk::BufferCopy copyRegion(0, 0, bufferSize);
    m_commandBuffer.copyBuffer(m_outputBuffer, stagingBuffer, 1, &copyRegion);
    m_commandBuffer.end();

    // 3. Submete e espera pelo comando de cópia
    vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, &m_commandBuffer);
    m_context.getDevice().resetFences(1, &m_fence);
    m_context.getComputeQueue().submit(1, &submitInfo, m_fence);
    (void)m_context.getDevice().waitForFences(1, &m_fence, VK_TRUE, UINT64_MAX);

    // 4. Mapeia a memória do buffer de staging para a CPU e salva o arquivo
    void* mappedData;
    vmaMapMemory(m_context.getAllocator(), stagingAllocation, &mappedData);

    std::ofstream file(filename);
    file << "P3\n" << m_imageWidth << " " << m_imageHeight << "\n255\n";
    float* pixels = static_cast<float*>(mappedData);

    for (uint32_t i = 0; i < m_imageWidth * m_imageHeight; ++i) {
        int r = static_cast<int>(pixels[i * 4 + 0] * 255.999);
        int g = static_cast<int>(pixels[i * 4 + 1] * 255.999);
        int b = static_cast<int>(pixels[i * 4 + 2] * 255.999);
        file << r << " " << g << " " << b << "\n";
    }
    file.close();

    // 5. Limpa os recursos de staging
    vmaUnmapMemory(m_context.getAllocator(), stagingAllocation);
    vmaDestroyBuffer(m_context.getAllocator(), stagingBuffer, stagingAllocation);

    Style::logDone("Image saved to " + filename);
}

void VulkanRenderer::createBuffers(const Camera& camera, const std::vector<GPUObject>& objects, const std::vector<GPUMaterial>& materials, const std::vector<GPULight>& lights) {
    // 1. Criar o buffer de saída
    vk::DeviceSize outputBufferSize = m_imageWidth * m_imageHeight * 4 * sizeof(float);
    vk::BufferCreateInfo outputBufferInfo({}, outputBufferSize, vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc, vk::SharingMode::eExclusive);
    VmaAllocationCreateInfo outputAllocInfo = {};
    outputAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    vmaCreateBuffer(m_context.getAllocator(), reinterpret_cast<const VkBufferCreateInfo*>(&outputBufferInfo), &outputAllocInfo, reinterpret_cast<VkBuffer*>(&m_outputBuffer), &m_outputBufferAllocation, nullptr);
    Style::logInfo("Output buffer created.");

    // 2. Criar e preencher o buffer da câmera
    GPUCameraData camData;
    camData.position[0] = camera.pos.x;
    camData.position[1] = camera.pos.y;
    camData.position[2] = camera.pos.z;

    camData.pixel_00_loc[0] = camera.pixel_00_loc().x;
    camData.pixel_00_loc[1] = camera.pixel_00_loc().y;
    camData.pixel_00_loc[2] = camera.pixel_00_loc().z;

    camData.view_u[0] = camera.pixel_delta_u().x;
    camData.view_u[1] = camera.pixel_delta_u().y;
    camData.view_u[2] = camera.pixel_delta_u().z;

    camData.view_v[0] = camera.pixel_delta_v().x;
    camData.view_v[1] = camera.pixel_delta_v().y;
    camData.view_v[2] = camera.pixel_delta_v().z;

    camData.ambient_color[0] = 0.2f;
    camData.ambient_color[1] = 0.2f;
    camData.ambient_color[2] = 0.2f;

    camData.image_width = m_imageWidth;
    camData.image_height = m_imageHeight;

    camData.samples_per_pixel = 16;

    vk::BufferCreateInfo cameraBufferInfo({}, sizeof(GPUCameraData), vk::BufferUsageFlagBits::eUniformBuffer, vk::SharingMode::eExclusive);
    VmaAllocationCreateInfo cameraAllocInfo = {};
    cameraAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU; // Buffer para transferir dados da CPU para a GPU

    vmaCreateBuffer(m_context.getAllocator(), reinterpret_cast<const VkBufferCreateInfo*>(&cameraBufferInfo), &cameraAllocInfo, reinterpret_cast<VkBuffer*>(&m_cameraBuffer), &m_cameraBufferAllocation, nullptr);

    void* mappedData;
    vmaMapMemory(m_context.getAllocator(), m_cameraBufferAllocation, &mappedData);
    memcpy(mappedData, &camData, sizeof(GPUCameraData));
    vmaUnmapMemory(m_context.getAllocator(), m_cameraBufferAllocation);
    Style::logInfo("Camera uniform buffer created and updated.");

    // Buffer de Objetos
    vk::DeviceSize objectBufferSize = sizeof(GPUObject) * objects.size();
    vk::BufferCreateInfo objectBufferInfo({}, objectBufferSize, vk::BufferUsageFlagBits::eStorageBuffer);
    VmaAllocationCreateInfo objectAllocInfo = { {}, VMA_MEMORY_USAGE_CPU_TO_GPU };
    vmaCreateBuffer(m_context.getAllocator(), reinterpret_cast<const VkBufferCreateInfo*>(&objectBufferInfo), &objectAllocInfo, reinterpret_cast<VkBuffer*>(&m_objectBuffer), &m_objectBufferAllocation, nullptr);
    vmaMapMemory(m_context.getAllocator(), m_objectBufferAllocation, &mappedData);
    memcpy(mappedData, objects.data(), objectBufferSize);
    vmaUnmapMemory(m_context.getAllocator(), m_objectBufferAllocation);
    Style::logInfo("Object storage buffer created.");

    // Buffer de Materiais
    vk::DeviceSize materialBufferSize = sizeof(GPUMaterial) * materials.size();
    vk::BufferCreateInfo materialBufferInfo({}, materialBufferSize, vk::BufferUsageFlagBits::eStorageBuffer);
    VmaAllocationCreateInfo materialAllocInfo = { {}, VMA_MEMORY_USAGE_CPU_TO_GPU };
    vmaCreateBuffer(m_context.getAllocator(), reinterpret_cast<const VkBufferCreateInfo*>(&materialBufferInfo), &materialAllocInfo, reinterpret_cast<VkBuffer*>(&m_materialBuffer), &m_materialBufferAllocation, nullptr);
    vmaMapMemory(m_context.getAllocator(), m_materialBufferAllocation, &mappedData);
    memcpy(mappedData, materials.data(), materialBufferSize);
    vmaUnmapMemory(m_context.getAllocator(), m_materialBufferAllocation);
    Style::logInfo("Material storage buffer created.");

    // Buffer de Iluminação
    vk::DeviceSize lightBufferSize = sizeof(GPULight) * lights.size();
    vk::BufferCreateInfo lightBufferInfo({}, lightBufferSize, vk::BufferUsageFlagBits::eStorageBuffer);
    VmaAllocationCreateInfo lightAllocInfo = { {}, VMA_MEMORY_USAGE_CPU_TO_GPU };
    vmaCreateBuffer(m_context.getAllocator(), reinterpret_cast<const VkBufferCreateInfo*>(&lightBufferInfo), &lightAllocInfo, reinterpret_cast<VkBuffer*>(&m_lightBuffer), &m_lightBufferAllocation, nullptr);
    vmaMapMemory(m_context.getAllocator(), m_lightBufferAllocation, &mappedData);
    memcpy(mappedData, lights.data(), lightBufferSize);
    vmaUnmapMemory(m_context.getAllocator(), m_lightBufferAllocation);
    Style::logInfo("Light storage buffer created.");
}

void VulkanRenderer::createDescriptorSet() {
    // 1. Definir os Bindings:
    std::array<vk::DescriptorSetLayoutBinding, 5> bindings = {
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute), // Output
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute), // Camera
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute), // Objects
        vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute),  // Materials
        vk::DescriptorSetLayoutBinding(4, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute) // Lights
    };
    vk::DescriptorSetLayoutCreateInfo layoutCreateInfo({}, static_cast<uint32_t>(bindings.size()), bindings.data());
    m_descriptorSetLayout = m_context.getDevice().createDescriptorSetLayout(layoutCreateInfo);

    // 2. Criar o Pool
    std::array<vk::DescriptorPoolSize, 2> poolSizes = {
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 4),
        vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, 1),
    };
    vk::DescriptorPoolCreateInfo poolCreateInfo({}, 1, static_cast<uint32_t>(poolSizes.size()), poolSizes.data());
    m_descriptorPool = m_context.getDevice().createDescriptorPool(poolCreateInfo);

    // 3. Alocar o Set
    vk::DescriptorSetAllocateInfo allocInfo(m_descriptorPool, 1, &m_descriptorSetLayout);
    m_descriptorSet = m_context.getDevice().allocateDescriptorSets(allocInfo)[0];

    // 4. Conectar os Buffers ao Set
    vk::DescriptorBufferInfo outputInfo(m_outputBuffer, 0, VK_WHOLE_SIZE);
    vk::DescriptorBufferInfo cameraInfo(m_cameraBuffer, 0, VK_WHOLE_SIZE);
    vk::DescriptorBufferInfo objectInfo(m_objectBuffer, 0, VK_WHOLE_SIZE);
    vk::DescriptorBufferInfo materialInfo(m_materialBuffer, 0, VK_WHOLE_SIZE);
    vk::DescriptorBufferInfo lightInfo(m_lightBuffer, 0, VK_WHOLE_SIZE);

    std::array<vk::WriteDescriptorSet, 5> writeSets = {
        vk::WriteDescriptorSet(m_descriptorSet, 0, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &outputInfo),
        vk::WriteDescriptorSet(m_descriptorSet, 1, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &cameraInfo),
        vk::WriteDescriptorSet(m_descriptorSet, 2, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &objectInfo),
        vk::WriteDescriptorSet(m_descriptorSet, 3, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &materialInfo),
        vk::WriteDescriptorSet(m_descriptorSet, 4, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &lightInfo)
    };
    m_context.getDevice().updateDescriptorSets(static_cast<uint32_t>(writeSets.size()), writeSets.data(), 0, nullptr);
    
    Style::logInfo("Descriptor Set created for all buffers.");
}

void VulkanRenderer::createComputePipeline(std::string shaderPath) {
    // Carrega o shader compilado (SPIR-V)
    std::ifstream file(shaderPath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Falha ao abrir o arquivo do shader!");
    }
    size_t fileSize = (size_t)file.tellg();
    std::vector<char> shaderCode(fileSize);
    file.seekg(0);
    file.read(shaderCode.data(), fileSize);
    file.close();

    vk::ShaderModuleCreateInfo createInfo({}, shaderCode.size(),
                                          reinterpret_cast<const uint32_t*>(shaderCode.data()));
    vk::ShaderModule computeShaderModule = m_context.getDevice().createShaderModule(createInfo);

    // Define o estágio do pipeline de computação
    vk::PipelineShaderStageCreateInfo shaderStageInfo({}, vk::ShaderStageFlagBits::eCompute,
                                                      computeShaderModule, "main");

    // Cria o layout do pipeline a partir do nosso descriptor set layout
    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo({}, 1, &m_descriptorSetLayout);
    m_pipelineLayout = m_context.getDevice().createPipelineLayout(pipelineLayoutCreateInfo);

    // Cria o pipeline de computação
    vk::ComputePipelineCreateInfo pipelineInfo({}, // flags
                                               shaderStageInfo, m_pipelineLayout);
    m_computePipeline = m_context.getDevice().createComputePipeline(nullptr, pipelineInfo).value;

    // O módulo do shader não é mais necessário após a criação do pipeline
    m_context.getDevice().destroyShaderModule(computeShaderModule);
    Style::logInfo("Compute pipeline created.");
}

void VulkanRenderer::createCommandObjects() {
    vk::CommandPoolCreateInfo poolInfo({}, m_context.getComputeQueueFamilyIndex());
    m_commandPool = m_context.getDevice().createCommandPool(poolInfo);

    vk::CommandBufferAllocateInfo allocInfo(m_commandPool, vk::CommandBufferLevel::ePrimary, 1);
    m_commandBuffer = m_context.getDevice().allocateCommandBuffers(allocInfo)[0];

    vk::FenceCreateInfo fenceCreateInfo(vk::FenceCreateFlagBits::eSignaled);
    m_fence = m_context.getDevice().createFence(fenceCreateInfo);

    Style::logInfo("Command pool and buffer created.");
}

} // namespace Prism