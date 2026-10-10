/*
 * Checks that NV12 sampled through an RGB_IDENTITY YCbCr conversion plus the
 * shader levels of nv12-projection.frag ("normal") gives the same RGB as the
 * former YCBCR_709 / ITU_NARROW sampler, including linear filtering and
 * chroma reconstruction. The upper half of the 64x64 test image holds random
 * codes (out of gamut too), the lower half legal limited-range values.
 *
 *   vulkan-nv12-levels nv12-levels.comp.spv
 */
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define W 64U
#define H 64U
#define GRID 256U
#define SAMPLES (GRID * GRID)

static void check(VkResult result, const char *what)
{
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s: VkResult=%d\n", what, result);
        exit(2);
    }
}

static uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits,
                            VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1U << i)) &&
            (properties.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    fprintf(stderr, "no memory type\n");
    exit(2);
}

static void *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        exit(2);
    }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    void *data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "read %s failed\n", path);
        exit(2);
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

struct host_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapping;
};

static struct host_buffer host_buffer(VkDevice device, VkPhysicalDevice physical,
                                      VkDeviceSize size,
                                      VkBufferUsageFlags usage)
{
    struct host_buffer b = {0};
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage,
    };
    check(vkCreateBuffer(device, &info, NULL, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, b.buffer, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(
            physical, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    check(vkAllocateMemory(device, &allocation, NULL, &b.memory),
          "vkAllocateMemory(buffer)");
    check(vkBindBufferMemory(device, b.buffer, b.memory, 0),
          "vkBindBufferMemory");
    check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapping),
          "vkMapMemory");
    return b;
}

static uint32_t random_state = 12345U;

static uint8_t random_byte(uint8_t low, uint8_t high)
{
    random_state = random_state * 1664525U + 1013904223U;
    return (uint8_t)(low + (random_state >> 8) % (uint32_t)(high - low + 1));
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s nv12-levels.comp.spv\n", argv[0]);
        return 2;
    }
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance;
    check(vkCreateInstance(&instance_info, NULL, &instance),
          "vkCreateInstance");
    uint32_t count = 1;
    VkPhysicalDevice physical;
    VkResult enumerated = vkEnumeratePhysicalDevices(instance, &count,
                                                     &physical);
    if (enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE)
        check(enumerated, "vkEnumeratePhysicalDevices");
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    printf("device=%s driver_version=0x%x\n", properties.deviceName,
           properties.driverVersion);

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    VkQueueFamilyProperties families[16];
    if (family_count > 16)
        family_count = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count,
                                             families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count && family == UINT32_MAX; ++i)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            family = i;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
        .samplerYcbcrConversion = VK_TRUE,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &ycbcr,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
    };
    VkDevice device;
    check(vkCreateDevice(physical, &device_info, NULL, &device),
          "vkCreateDevice");
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    const VkFormat format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image;
    check(vkCreateImage(device, &image_info, NULL, &image), "vkCreateImage");
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo image_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(physical, requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VkDeviceMemory image_memory;
    check(vkAllocateMemory(device, &image_allocation, NULL, &image_memory),
          "vkAllocateMemory(image)");
    check(vkBindImageMemory(device, image, image_memory, 0),
          "vkBindImageMemory");

    /* Test data: random codes above, legal limited range below. */
    const size_t luma_size = W * H, chroma_size = W * H / 2;
    struct host_buffer staging = host_buffer(
        device, physical, luma_size + chroma_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    uint8_t *bytes = staging.mapping;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
            bytes[y * W + x] = y < H / 2 ? random_byte(0, 255)
                                         : random_byte(16, 235);
    for (uint32_t y = 0; y < H / 2; ++y)
        for (uint32_t x = 0; x < W; ++x)
            bytes[luma_size + y * W + x] = y < H / 4 ? random_byte(0, 255)
                                                     : random_byte(16, 240);
    struct host_buffer output = host_buffer(
        device, physical, 2U * SAMPLES * 4U * sizeof(float),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    size_t code_size = 0;
    uint32_t *code = read_file(argv[1], &code_size);
    VkShaderModuleCreateInfo module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code_size, .pCode = code,
    };
    VkShaderModule module;
    check(vkCreateShaderModule(device, &module_info, NULL, &module),
          "vkCreateShaderModule");

    VkDescriptorPoolSize pool_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 2, .pPoolSizes = pool_sizes,
    };
    VkDescriptorPool pool;
    check(vkCreateDescriptorPool(device, &pool_info, NULL, &pool),
          "vkCreateDescriptorPool");

    VkPipeline pipelines[2];
    VkPipelineLayout layouts[2];
    VkDescriptorSet sets[2];
    for (int mode = 0; mode < 2; ++mode) {
        VkSamplerYcbcrConversionCreateInfo conversion_info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
            .format = format,
            .ycbcrModel = mode == 0
                              ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709
                              : VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY,
            .ycbcrRange = mode == 0 ? VK_SAMPLER_YCBCR_RANGE_ITU_NARROW
                                    : VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
            .xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
            .yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
            .chromaFilter = VK_FILTER_LINEAR,
        };
        VkSamplerYcbcrConversion conversion;
        check(vkCreateSamplerYcbcrConversion(device, &conversion_info, NULL,
                                             &conversion),
              "vkCreateSamplerYcbcrConversion");
        VkSamplerYcbcrConversionInfo attach = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
            .conversion = conversion,
        };
        VkSamplerCreateInfo sampler_info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = &attach,
            .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        };
        VkSampler sampler;
        check(vkCreateSampler(device, &sampler_info, NULL, &sampler),
              "vkCreateSampler");
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = &attach, .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkImageView view;
        check(vkCreateImageView(device, &view_info, NULL, &view),
              "vkCreateImageView");
        VkDescriptorSetLayoutBinding bindings[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT, &sampler},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        };
        VkDescriptorSetLayoutCreateInfo set_layout_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2, .pBindings = bindings,
        };
        VkDescriptorSetLayout set_layout;
        check(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL,
                                          &set_layout),
              "vkCreateDescriptorSetLayout");
        VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo layout_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &set_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
        };
        check(vkCreatePipelineLayout(device, &layout_info, NULL,
                                     &layouts[mode]),
              "vkCreatePipelineLayout");
        VkComputePipelineCreateInfo pipeline_info = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main",
                      NULL},
            .layout = layouts[mode],
        };
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                       &pipeline_info, NULL,
                                       &pipelines[mode]),
              "vkCreateComputePipelines");
        VkDescriptorSetAllocateInfo set_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool, .descriptorSetCount = 1,
            .pSetLayouts = &set_layout,
        };
        check(vkAllocateDescriptorSets(device, &set_info, &sets[mode]),
              "vkAllocateDescriptorSets");
        VkDescriptorImageInfo image_descriptor = {
            .imageView = view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkDescriptorBufferInfo buffer_descriptor = {
            output.buffer, 0, VK_WHOLE_SIZE,
        };
        VkWriteDescriptorSet writes[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = sets[mode], .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &image_descriptor},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = sets[mode], .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .pBufferInfo = &buffer_descriptor},
        };
        vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    }

    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family,
    };
    VkCommandPool command_pool;
    check(vkCreateCommandPool(device, &command_pool_info, NULL,
                              &command_pool),
          "vkCreateCommandPool");
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer command;
    check(vkAllocateCommandBuffers(device, &command_info, &command),
          "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
    VkImageMemoryBarrier to_transfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &to_transfer);
    VkBufferImageCopy copies[2] = {
        {.bufferOffset = 0,
         .imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1},
         .imageExtent = {W, H, 1}},
        {.bufferOffset = luma_size,
         .imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1},
         .imageExtent = {W / 2, H / 2, 1}},
    };
    vkCmdCopyBufferToImage(command, staging.buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, copies);
    VkImageMemoryBarrier to_shader = to_transfer;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &to_shader);
    for (int mode = 0; mode < 2; ++mode) {
        int32_t push[2] = {mode, mode * (int32_t)SAMPLES};
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelines[mode]);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                layouts[mode], 0, 1, &sets[mode], 0, NULL);
        vkCmdPushConstants(command, layouts[mode],
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           push);
        vkCmdDispatch(command, GRID / 16U, GRID / 16U, 1);
    }
    VkMemoryBarrier to_host = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL,
                         0, NULL);
    check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command,
    };
    check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

    /* Compare per region; rows near the boundary mix both halves. */
    const float *result = output.mapping;
    double max_random = 0.0, max_legal = 0.0, sum_legal = 0.0;
    double min_old = 1e9, max_old = -1e9;
    uint64_t legal = 0, random_count = 0;
    for (uint32_t y = 0; y < GRID; ++y) {
        bool random_rows = y < GRID / 2U - 8U;
        bool legal_rows = y >= GRID / 2U + 8U;
        if (!random_rows && !legal_rows)
            continue;
        for (uint32_t x = 0; x < GRID; ++x)
            for (uint32_t c = 0; c < 3; ++c) {
                float old_value = result[(y * GRID + x) * 4U + c];
                float new_value = result[(SAMPLES + y * GRID + x) * 4U + c];
                min_old = fmin(min_old, old_value);
                max_old = fmax(max_old, old_value);
                double diff = fabs(fmin(fmax(old_value, 0.0f), 1.0f) -
                                   new_value);
                if (random_rows) {
                    max_random = fmax(max_random, diff);
                    ++random_count;
                } else {
                    max_legal = fmax(max_legal, diff);
                    sum_legal += diff;
                    ++legal;
                }
            }
    }
    printf("old_sampler_range=%.4f..%.4f (values outside 0..1: unclamped)\n",
           min_old, max_old);
    printf("legal max_diff=%.6f (%.3f of 8 bit) mean_diff=%.7f samples=%llu\n",
           max_legal, max_legal * 255.0, sum_legal / (double)legal,
           (unsigned long long)legal);
    printf("random max_diff=%.6f (%.3f of 8 bit) samples=%llu\n", max_random,
           max_random * 255.0, (unsigned long long)random_count);
    bool pass = max_legal * 255.0 < 0.5;
    printf("result=%s\n", pass ? "match" : "mismatch");
    return pass ? 0 : 1;
}
