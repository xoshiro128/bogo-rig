extern "C" {
#include "compute.h"
}
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <vector>

#include "compute_spv.h"

static VkInstance instance = VK_NULL_HANDLE;
static VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
static VkDevice device = VK_NULL_HANDLE;
static VkQueue computeQueue = VK_NULL_HANDLE;
static VkCommandPool commandPool = VK_NULL_HANDLE;
static VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
static VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
static VkPipeline computePipeline = VK_NULL_HANDLE;
static VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
static VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
static VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
static VkBuffer paramBuffer = VK_NULL_HANDLE;
static VkDeviceMemory paramBufferMemory = VK_NULL_HANDLE;
static VkBuffer resultBuffer = VK_NULL_HANDLE;
static VkDeviceMemory resultBufferMemory = VK_NULL_HANDLE;
static uint32_t current_max_workgroups = 0;
static bool vulkan_initialized = false;
static uint32_t queueFamilyIndex = 0;

static uint32_t find_memory_type(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    fprintf(stderr, "Failed to find suitable memory type\n");
    exit(1);
}

static void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create buffer\n");
        exit(1);
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = find_memory_type(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate buffer memory\n");
        exit(1);
    }

    vkBindBufferMemory(device, buffer, bufferMemory, 0);
}

static void init_vulkan() {
    if (vulkan_initialized) return;

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "ComputeApp";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create Vulkan instance\n");
        exit(1);
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    for (const auto& dev : devices) {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qFamilies(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qFamilies.data());

        for (uint32_t i = 0; i < qCount; i++) {
            if (qFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                physicalDevice = dev;
                queueFamilyIndex = i;
                break;
            }
        }
        if (physicalDevice != VK_NULL_HANDLE) break;
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        fprintf(stderr, "Failed to find a suitable GPU with compute support\n");
        exit(1);
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.shaderInt64 = VK_TRUE;

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pEnabledFeatures = &deviceFeatures;

    if (vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create logical device\n");
        exit(1);
    }

    vkGetDeviceQueue(device, queueFamilyIndex, 0, &computeQueue);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    // --- EMBEDDED SHADER MODULE CREATION ---
    VkShaderModuleCreateInfo shaderModuleCreateInfo{};
    shaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleCreateInfo.codeSize = build_compute_spv_len;
    // SPIR-V is guaranteed to be aligned to 4 bytes, so this cast is safe and standard
    shaderModuleCreateInfo.pCode = reinterpret_cast<const uint32_t*>(build_compute_spv);

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &shaderModuleCreateInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create shader module from embedded data\n");
        exit(1);
    }
    // ---------------------------------------

    VkDescriptorSetLayoutBinding bindings[2];
    bindings[0].binding = 0; bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[0].descriptorCount = 1; bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1; bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[1].descriptorCount = 1; bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
    vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout);

    VkPipelineShaderStageCreateInfo shaderStageInfo{};
    shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStageInfo.module = shaderModule;
    shaderStageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStageInfo;
    pipelineInfo.layout = pipelineLayout;
    vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &computePipeline);

    // Clean up the temporary shader module (the pipeline has already compiled it)
    vkDestroyShaderModule(device, shaderModule, nullptr);

    VkDescriptorPoolSize poolSizes[1];
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfoDesc{};
    poolInfoDesc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfoDesc.poolSizeCount = 1;
    poolInfoDesc.pPoolSizes = poolSizes;
    poolInfoDesc.maxSets = 1;
    vkCreateDescriptorPool(device, &poolInfoDesc, nullptr, &descriptorPool);

    VkDescriptorSetAllocateInfo allocInfoDesc{};
    allocInfoDesc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfoDesc.descriptorPool = descriptorPool;
    allocInfoDesc.descriptorSetCount = 1;
    allocInfoDesc.pSetLayouts = &descriptorSetLayout;
    vkAllocateDescriptorSets(device, &allocInfoDesc, &descriptorSet);

    vulkan_initialized = true;
}

extern "C" compute_result compute_run(uint64_t seed, uint64_t lo, uint64_t hi) {
    compute_result best = { .best_correct = -1, .best_arr = {0}, .best_index = lo };
    for(int i = 0; i < 25; ++i) best.best_arr[i] = i + 1;
    if (hi <= lo) return best;

    init_vulkan();

    uint64_t total_work = hi - lo;
    uint32_t workgroup_size = 256;
    uint32_t workgroups = (total_work + workgroup_size - 1) / workgroup_size;
    if (workgroups > 4096) workgroups = 4096;

    if (workgroups > current_max_workgroups) {
        if (paramBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, paramBuffer, nullptr);
            vkFreeMemory(device, paramBufferMemory, nullptr);
            vkDestroyBuffer(device, resultBuffer, nullptr);
            vkFreeMemory(device, resultBufferMemory, nullptr);
        }

        create_buffer(32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, paramBuffer, paramBufferMemory);

        VkDeviceSize resultSize = workgroups * 128; // 128 bytes per WGResult
        create_buffer(resultSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, resultBuffer, resultBufferMemory);

        VkDescriptorBufferInfo bufferInfos[2];
        bufferInfos[0].buffer = paramBuffer; bufferInfos[0].offset = 0; bufferInfos[0].range = 32;
        bufferInfos[1].buffer = resultBuffer; bufferInfos[1].offset = 0; bufferInfos[1].range = resultSize;

        VkWriteDescriptorSet descriptorWrites[2];
        descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; descriptorWrites[0].dstSet = descriptorSet; descriptorWrites[0].dstBinding = 0; descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; descriptorWrites[0].descriptorCount = 1; descriptorWrites[0].pBufferInfo = &bufferInfos[0];
        descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; descriptorWrites[1].dstSet = descriptorSet; descriptorWrites[1].dstBinding = 1; descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; descriptorWrites[1].descriptorCount = 1; descriptorWrites[1].pBufferInfo = &bufferInfos[1];

        vkUpdateDescriptorSets(device, 2, descriptorWrites, 0, nullptr);
        current_max_workgroups = workgroups;
    }

    struct Params { uint64_t seed; uint64_t base_index; uint64_t total_work; };
    void* data;
    vkMapMemory(device, paramBufferMemory, 0, sizeof(Params), 0, &data);
    Params* params = (Params*)data;
    params->seed = seed;
    params->base_index = lo;
    params->total_work = total_work;
    vkUnmapMemory(device, paramBufferMemory);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    vkCmdDispatch(commandBuffer, workgroups, 1, 1);
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vkCreateFence(device, &fenceInfo, nullptr, &fence);

    vkQueueSubmit(computeQueue, 1, &submitInfo, fence);
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device, fence, nullptr);

    vkMapMemory(device, resultBufferMemory, 0, workgroups * 128, 0, &data);
    uint8_t* resultData = (uint8_t*)data;

    int global_best_correct = -1;
    uint64_t global_best_index = lo;
    int global_best_arr[25];
    for(int i = 0; i < 25; ++i) global_best_arr[i] = i + 1;

    for (uint32_t i = 0; i < workgroups; i++) {
        uint32_t* wg_result = (uint32_t*)(resultData + i * 128);
        uint32_t best_c = wg_result[0];
        uint64_t best_i = *((uint64_t*)(wg_result + 2));
        uint32_t* arr = wg_result + 4;

        if ((int)best_c > global_best_correct) {
            global_best_correct = best_c;
            global_best_index = best_i;
            for (int p = 0; p < 25; p++) global_best_arr[p] = arr[p];
            if (global_best_correct == 25) break;
        }
    }

    vkUnmapMemory(device, resultBufferMemory);

    best.best_correct = global_best_correct;
    best.best_index = global_best_index;
    for (int i = 0; i < 25; i++) best.best_arr[i] = global_best_arr[i];

    return best;
}
