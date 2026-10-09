/* Minimal visible OpenXR/Vulkan projection-layer probe for Steam Frame.
 * It intentionally contains no video decoder.  Its purpose is to verify the
 * OpenXR session, Vulkan device chosen by the runtime, swapchain ownership,
 * view projection and a LOCAL-space 2:1 quad before video is introduced. */
#define _POSIX_C_SOURCE 200809L
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_XLIB

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_IMAGES 8U
#define UBO_STRIDE 256U

struct mat4 { float v[16]; };

struct app {
    XrInstance xr;
    XrSystemId system;
    XrSession session;
    XrSpace space;
    XrSessionState state;
    bool running;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkFence fence;
    VkRenderPass render_pass;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkBuffer uniform_buffer;
    VkDeviceMemory uniform_memory;
    void *uniform_mapping;
    VkFormat color_format;
    XrSwapchain swapchain;
    uint32_t width, height, image_count;
    XrSwapchainImageVulkan2KHR images[MAX_IMAGES];
    VkImageView image_views[MAX_IMAGES][2];
    VkFramebuffer framebuffers[MAX_IMAGES][2];
    uint64_t xr_frames, rendered_frames, session_changes;
};

static void die_xr(const char *what, XrResult result)
{
    fprintf(stderr, "%s: XrResult=%d\n", what, result);
    exit(2);
}

static void need_xr(const char *what, XrResult result)
{
    if (XR_FAILED(result)) die_xr(what, result);
}

static void need_vk(const char *what, VkResult result)
{
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s: VkResult=%d\n", what, result);
        exit(2);
    }
}

static void need(bool condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "%s\n", what);
        exit(2);
    }
}

static uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits,
                            VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1U << i)) &&
            (properties.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    fprintf(stderr, "no compatible Vulkan memory type\n");
    exit(2);
}

static struct mat4 multiply(struct mat4 a, struct mat4 b)
{
    struct mat4 result = {{0}};
    for (unsigned col = 0; col < 4; ++col)
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned k = 0; k < 4; ++k)
                result.v[col * 4 + row] += a.v[k * 4 + row] * b.v[col * 4 + k];
    return result;
}

static struct mat4 view_from_pose(XrPosef pose)
{
    float x = pose.orientation.x, y = pose.orientation.y;
    float z = pose.orientation.z, w = pose.orientation.w;
    /* Invert the rigid OpenXR view pose.  The rotational part of the view
     * matrix is the transpose of the pose's quaternion rotation. */
    struct mat4 result = {{
        1 - 2*y*y - 2*z*z, 2*x*y - 2*z*w,     2*x*z + 2*y*w,     0,
        2*x*y + 2*z*w,     1 - 2*x*x - 2*z*z, 2*y*z - 2*x*w,     0,
        2*x*z - 2*y*w,     2*y*z + 2*x*w,     1 - 2*x*x - 2*y*y, 0,
        0,                 0,                 0,                 1,
    }};
    result.v[12] = -(result.v[0] * pose.position.x + result.v[4] * pose.position.y + result.v[8] * pose.position.z);
    result.v[13] = -(result.v[1] * pose.position.x + result.v[5] * pose.position.y + result.v[9] * pose.position.z);
    result.v[14] = -(result.v[2] * pose.position.x + result.v[6] * pose.position.y + result.v[10] * pose.position.z);
    return result;
}

static struct mat4 projection_from_fov(XrFovf fov)
{
    const float near_z = 0.05f, far_z = 100.0f;
    float left = tanf(fov.angleLeft), right = tanf(fov.angleRight);
    float down = tanf(fov.angleDown), up = tanf(fov.angleUp);
    struct mat4 result = {{0}};
    result.v[0] = 2.0f / (right - left);
    /* Vulkan's framebuffer Y direction is opposite to OpenXR's view-space
     * Y direction when a positive viewport height is used. */
    result.v[5] = 2.0f / (down - up);
    result.v[8] = (right + left) / (right - left);
    result.v[9] = (up + down) / (down - up);
    result.v[10] = far_z / (near_z - far_z);
    result.v[11] = -1.0f;
    result.v[14] = -(far_z * near_z) / (far_z - near_z);
    return result;
}

static void poll_events(struct app *app)
{
    for (;;) {
        XrEventDataBuffer event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result = xrPollEvent(app->xr, &event);
        if (result == XR_EVENT_UNAVAILABLE) return;
        need_xr("xrPollEvent", result);
        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
        XrEventDataSessionStateChanged *change =
            (XrEventDataSessionStateChanged *)&event;
        app->state = change->state;
        ++app->session_changes;
        printf("openxr_session_state=%d time=%" PRId64 "\n", app->state,
               change->time);
        if (app->state == XR_SESSION_STATE_READY) {
            XrSessionBeginInfo begin = {
                .type = XR_TYPE_SESSION_BEGIN_INFO,
                .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            };
            need_xr("xrBeginSession", xrBeginSession(app->session, &begin));
            app->running = true;
        } else if (app->state == XR_SESSION_STATE_STOPPING) {
            need_xr("xrEndSession", xrEndSession(app->session));
            app->running = false;
        } else if (app->state == XR_SESSION_STATE_EXITING ||
                   app->state == XR_SESSION_STATE_LOSS_PENDING) {
            app->running = false;
            return;
        }
    }
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); exit(2); }
    if (fseek(file, 0, SEEK_END) || (*size = (size_t)ftell(file)) == 0 ||
        fseek(file, 0, SEEK_SET)) { perror("read shader size"); exit(2); }
    uint8_t *data = malloc(*size);
    if (!data || fread(data, 1, *size, file) != *size) {
        perror("read shader"); exit(2);
    }
    fclose(file);
    return data;
}

static VkShaderModule shader(VkDevice device, const char *path)
{
    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    need(size % 4 == 0, "SPIR-V size is not word aligned");
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size, .pCode = (const uint32_t *)data,
    };
    VkShaderModule result = VK_NULL_HANDLE;
    need_vk("vkCreateShaderModule", vkCreateShaderModule(device, &info, NULL, &result));
    free(data);
    return result;
}

static void initialize_xr_vulkan(struct app *app)
{
    const char *extensions[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo info = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {.applicationVersion = 1, .engineVersion = 1,
                            .apiVersion = XR_MAKE_VERSION(1, 0, 0)},
        .enabledExtensionCount = ARRAY_COUNT(extensions),
        .enabledExtensionNames = extensions,
    };
    snprintf(info.applicationInfo.applicationName,
             sizeof(info.applicationInfo.applicationName), "stereonaut-xr-quad");
    snprintf(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName), "stereonaut");
    need_xr("xrCreateInstance", xrCreateInstance(&info, &app->xr));
    XrSystemGetInfo system_info = {.type = XR_TYPE_SYSTEM_GET_INFO,
                                   .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    need_xr("xrGetSystem", xrGetSystem(app->xr, &system_info, &app->system));

    PFN_xrCreateVulkanInstanceKHR create_instance = NULL;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device = NULL;
    PFN_xrCreateVulkanDeviceKHR create_device = NULL;
    PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = NULL;
    need_xr("xrGetInstanceProcAddr(create instance)", xrGetInstanceProcAddr(app->xr, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&create_instance));
    need_xr("xrGetInstanceProcAddr(get device)", xrGetInstanceProcAddr(app->xr, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&get_device));
    need_xr("xrGetInstanceProcAddr(create device)", xrGetInstanceProcAddr(app->xr, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&create_device));
    need_xr("xrGetInstanceProcAddr(requirements)", xrGetInstanceProcAddr(app->xr, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&get_requirements));
    need(create_instance && get_device && create_device && get_requirements, "missing Vulkan-enable2 entry point");
    XrGraphicsRequirementsVulkanKHR requirements = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR,
    };
    need_xr("xrGetVulkanGraphicsRequirements2KHR", get_requirements(app->xr, app->system, &requirements));
    printf("openxr_vulkan_requirements min=%" PRIu64 " max=%" PRIu64 "\n",
           requirements.minApiVersionSupported, requirements.maxApiVersionSupported);

    VkApplicationInfo vk_app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "stereonaut-xr-quad", .applicationVersion = 1,
        .pEngineName = "stereonaut", .engineVersion = 1, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo vk_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &vk_app};
    XrVulkanInstanceCreateInfoKHR xr_vk_info = {
        .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = app->system,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanCreateInfo = &vk_info};
    VkResult vk_result = VK_SUCCESS;
    need_xr("xrCreateVulkanInstanceKHR", create_instance(app->xr, &xr_vk_info, &app->instance, &vk_result));
    need_vk("xrCreateVulkanInstanceKHR Vulkan", vk_result);
    XrVulkanGraphicsDeviceGetInfoKHR device_info = {
        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
        .systemId = app->system, .vulkanInstance = app->instance};
    need_xr("xrGetVulkanGraphicsDevice2KHR", get_device(app->xr, &device_info, &app->physical));

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(app->physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    need(families != NULL, "allocate queue families");
    vkGetPhysicalDeviceQueueFamilyProperties(app->physical, &count, families);
    bool found = false;
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            app->queue_family = i; found = true; break;
        }
    free(families);
    need(found, "OpenXR Vulkan device has no graphics queue");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = app->queue_family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo vk_device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
    XrVulkanDeviceCreateInfoKHR xr_device_info = {
        .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = app->system,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanPhysicalDevice = app->physical,
        .vulkanCreateInfo = &vk_device_info};
    need_xr("xrCreateVulkanDeviceKHR", create_device(app->xr, &xr_device_info, &app->device, &vk_result));
    need_vk("xrCreateVulkanDeviceKHR Vulkan", vk_result);
    vkGetDeviceQueue(app->device, app->queue_family, 0, &app->queue);
    need(app->queue != VK_NULL_HANDLE, "vkGetDeviceQueue returned null");
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(app->physical, &properties);
    printf("openxr_vulkan_device=%s queue_family=%u\n", properties.deviceName, app->queue_family);
}

static void initialize_swapchain(struct app *app)
{
    uint32_t format_count = 0;
    need_xr("xrEnumerateSwapchainFormats(count)", xrEnumerateSwapchainFormats(app->session, 0, &format_count, NULL));
    int64_t *formats = calloc(format_count, sizeof(*formats));
    need(formats != NULL, "allocate swapchain formats");
    need_xr("xrEnumerateSwapchainFormats(list)", xrEnumerateSwapchainFormats(app->session, format_count, &format_count, formats));
    app->color_format = VK_FORMAT_B8G8R8A8_SRGB;
    bool supported = false;
    for (uint32_t i = 0; i < format_count; ++i) if (formats[i] == app->color_format) supported = true;
    if (!supported) { app->color_format = (VkFormat)formats[0]; supported = true; }
    free(formats);
    XrViewConfigurationView views[2] = {
        {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}, {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t view_count = 0;
    need_xr("xrEnumerateViewConfigurationViews", xrEnumerateViewConfigurationViews(app->xr, app->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &view_count, views));
    need(view_count == 2, "Steam Frame did not report stereo views");
    app->width = views[0].recommendedImageRectWidth;
    app->height = views[0].recommendedImageRectHeight;
    need(views[1].recommendedImageRectWidth == app->width && views[1].recommendedImageRectHeight == app->height, "eyes have different recommended dimensions");
    XrSwapchainCreateInfo info = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
        .format = app->color_format, .sampleCount = 1, .width = app->width,
        .height = app->height, .faceCount = 1, .arraySize = 2, .mipCount = 1};
    need_xr("xrCreateSwapchain", xrCreateSwapchain(app->session, &info, &app->swapchain));
    need_xr("xrEnumerateSwapchainImages(count)", xrEnumerateSwapchainImages(app->swapchain, 0, &app->image_count, NULL));
    need(app->image_count > 0 && app->image_count <= MAX_IMAGES, "unexpected OpenXR swapchain image count");
    for (uint32_t i = 0; i < app->image_count; ++i) app->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    need_xr("xrEnumerateSwapchainImages(list)", xrEnumerateSwapchainImages(app->swapchain, app->image_count, &app->image_count, (XrSwapchainImageBaseHeader *)app->images));
    printf("openxr_swapchain format=%d dimensions=%ux%u images=%u\n", app->color_format, app->width, app->height, app->image_count);
}

static void create_pipeline(struct app *app)
{
    VkAttachmentDescription attachment = {.format = app->color_format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference reference = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference};
    VkRenderPassCreateInfo render_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass};
    need_vk("vkCreateRenderPass", vkCreateRenderPass(app->device, &render_info, NULL, &app->render_pass));
    for (uint32_t image = 0; image < app->image_count; ++image) for (uint32_t eye = 0; eye < 2; ++eye) {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = app->images[image].image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = app->color_format,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .baseArrayLayer = eye, .layerCount = 1}};
        need_vk("vkCreateImageView", vkCreateImageView(app->device, &view, NULL, &app->image_views[image][eye]));
        VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = app->render_pass, .attachmentCount = 1, .pAttachments = &app->image_views[image][eye], .width = app->width, .height = app->height, .layers = 1};
        need_vk("vkCreateFramebuffer", vkCreateFramebuffer(app->device, &fb, NULL, &app->framebuffers[image][eye]));
    }
    VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT};
    VkDescriptorSetLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    need_vk("vkCreateDescriptorSetLayout", vkCreateDescriptorSetLayout(app->device, &layout, NULL, &app->descriptor_layout));
    VkPipelineLayoutCreateInfo pipeline_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &app->descriptor_layout};
    need_vk("vkCreatePipelineLayout", vkCreatePipelineLayout(app->device, &pipeline_layout, NULL, &app->pipeline_layout));
    VkShaderModule vert = shader(app->device, "quad.vert.spv"), frag = shader(app->device, "quad.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"}, {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    VkViewport viewport = {.width = (float)app->width, .height = (float)app->height, .minDepth = 0, .maxDepth = 1};
    VkRect2D scissor = {.extent = {app->width, app->height}};
    VkPipelineViewportStateCreateInfo view = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend_attachment};
    VkGraphicsPipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly, .pViewportState = &view, .pRasterizationState = &raster, .pMultisampleState = &multisample, .pColorBlendState = &blend, .layout = app->pipeline_layout, .renderPass = app->render_pass};
    need_vk("vkCreateGraphicsPipelines", vkCreateGraphicsPipelines(app->device, VK_NULL_HANDLE, 1, &pipeline, NULL, &app->pipeline));
    vkDestroyShaderModule(app->device, vert, NULL); vkDestroyShaderModule(app->device, frag, NULL);
}

static void create_render_resources(struct app *app)
{
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = app->queue_family};
    need_vk("vkCreateCommandPool", vkCreateCommandPool(app->device, &pool, NULL, &app->command_pool));
    VkCommandBufferAllocateInfo commands = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    need_vk("vkAllocateCommandBuffers", vkAllocateCommandBuffers(app->device, &commands, &app->command));
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    need_vk("vkCreateFence", vkCreateFence(app->device, &fence, NULL, &app->fence));
    create_pipeline(app);
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 2 * UBO_STRIDE, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    need_vk("vkCreateBuffer(uniform)", vkCreateBuffer(app->device, &buffer, NULL, &app->uniform_buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(app->device, app->uniform_buffer, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(app->physical, requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    need_vk("vkAllocateMemory(uniform)", vkAllocateMemory(app->device, &allocation, NULL, &app->uniform_memory));
    need_vk("vkBindBufferMemory(uniform)", vkBindBufferMemory(app->device, app->uniform_buffer, app->uniform_memory, 0));
    need_vk("vkMapMemory(uniform)", vkMapMemory(app->device, app->uniform_memory, 0, VK_WHOLE_SIZE, 0, &app->uniform_mapping));
    VkDescriptorPoolSize size = {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = 1};
    VkDescriptorPoolCreateInfo descriptor_pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    need_vk("vkCreateDescriptorPool", vkCreateDescriptorPool(app->device, &descriptor_pool, NULL, &app->descriptor_pool));
    VkDescriptorSetAllocateInfo descriptor_allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = app->descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &app->descriptor_layout};
    need_vk("vkAllocateDescriptorSets", vkAllocateDescriptorSets(app->device, &descriptor_allocate, &app->descriptor_set));
    VkDescriptorBufferInfo descriptor_buffer = {.buffer = app->uniform_buffer, .offset = 0, .range = sizeof(struct mat4)};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = app->descriptor_set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &descriptor_buffer};
    vkUpdateDescriptorSets(app->device, 1, &write, 0, NULL);
}

static void render_frame(struct app *app)
{
    XrFrameState frame_state = {.type = XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wait = {.type = XR_TYPE_FRAME_WAIT_INFO};
    need_xr("xrWaitFrame", xrWaitFrame(app->session, &wait, &frame_state));
    XrFrameBeginInfo begin = {.type = XR_TYPE_FRAME_BEGIN_INFO};
    need_xr("xrBeginFrame", xrBeginFrame(app->session, &begin));
    ++app->xr_frames;
    if (!frame_state.shouldRender) {
        XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = frame_state.predictedDisplayTime,
            .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
        need_xr("xrEndFrame(empty)", xrEndFrame(app->session, &end));
        return;
    }
    XrView views[2] = {{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
    XrViewState view_state = {.type = XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    XrViewLocateInfo locate = {.type = XR_TYPE_VIEW_LOCATE_INFO,
        .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        .displayTime = frame_state.predictedDisplayTime, .space = app->space};
    need_xr("xrLocateViews", xrLocateViews(app->session, &locate, &view_state, 2, &count, views));
    need(count == 2 && (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
             (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT), "OpenXR returned no valid stereo pose");
    XrSwapchainImageAcquireInfo acquire = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t image_index = 0;
    need_xr("xrAcquireSwapchainImage", xrAcquireSwapchainImage(app->swapchain, &acquire, &image_index));
    XrSwapchainImageWaitInfo image_wait = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
    need_xr("xrWaitSwapchainImage", xrWaitSwapchainImage(app->swapchain, &image_wait));
    need(image_index < app->image_count, "invalid OpenXR swapchain index");
    for (uint32_t eye = 0; eye < 2; ++eye) {
        struct mat4 projection = projection_from_fov(views[eye].fov);
        struct mat4 view = view_from_pose(views[eye].pose);
        *(struct mat4 *)((uint8_t *)app->uniform_mapping + eye * UBO_STRIDE) = multiply(projection, view);
    }
    need_vk("vkWaitForFences(render)", vkWaitForFences(app->device, 1, &app->fence, VK_TRUE, UINT64_C(5000000000)));
    need_vk("vkResetFences(render)", vkResetFences(app->device, 1, &app->fence));
    need_vk("vkResetCommandPool(render)", vkResetCommandPool(app->device, app->command_pool, 0));
    VkCommandBufferBeginInfo command_begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    need_vk("vkBeginCommandBuffer(render)", vkBeginCommandBuffer(app->command, &command_begin));
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image = app->images[image_index].image, .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 2}};
    vkCmdPipelineBarrier(app->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        VkClearValue clear = {.color = {{0.005f, 0.005f, 0.008f, 1.0f}}};
        VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = app->render_pass, .framebuffer = app->framebuffers[image_index][eye],
            .renderArea = {.extent = {app->width, app->height}}, .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(app->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(app->command, VK_PIPELINE_BIND_POINT_GRAPHICS, app->pipeline);
        uint32_t offset = eye * UBO_STRIDE;
        vkCmdBindDescriptorSets(app->command, VK_PIPELINE_BIND_POINT_GRAPHICS, app->pipeline_layout, 0, 1, &app->descriptor_set, 1, &offset);
        vkCmdDraw(app->command, 4, 1, 0, 0);
        vkCmdEndRenderPass(app->command);
    }
    need_vk("vkEndCommandBuffer(render)", vkEndCommandBuffer(app->command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &app->command};
    need_vk("vkQueueSubmit(render)", vkQueueSubmit(app->queue, 1, &submit, app->fence));
    need_vk("vkWaitForFences(render completion)", vkWaitForFences(app->device, 1, &app->fence, VK_TRUE, UINT64_C(5000000000)));
    XrSwapchainImageReleaseInfo release = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    need_xr("xrReleaseSwapchainImage", xrReleaseSwapchainImage(app->swapchain, &release));
    XrCompositionLayerProjectionView projection_views[2] = {{.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (uint32_t eye = 0; eye < 2; ++eye) {
        projection_views[eye].pose = views[eye].pose; projection_views[eye].fov = views[eye].fov;
        projection_views[eye].subImage = (XrSwapchainSubImage){.swapchain = app->swapchain, .imageRect = {.extent = {(int32_t)app->width, (int32_t)app->height}}, .imageArrayIndex = eye};
    }
    XrCompositionLayerProjection layer = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = app->space, .viewCount = 2, .views = projection_views};
    const XrCompositionLayerBaseHeader *layers[] = {(const XrCompositionLayerBaseHeader *)&layer};
    XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = frame_state.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 1, .layers = layers};
    need_xr("xrEndFrame", xrEndFrame(app->session, &end));
    ++app->rendered_frames;
}

int main(void)
{
    struct app app = {0};
    initialize_xr_vulkan(&app);
    XrGraphicsBindingVulkan2KHR binding = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = app.instance, .physicalDevice = app.physical, .device = app.device, .queueFamilyIndex = app.queue_family, .queueIndex = 0};
    XrSessionCreateInfo session_info = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = app.system};
    need_xr("xrCreateSession", xrCreateSession(app.xr, &session_info, &app.session));
    /* SteamVR on Frame requires READY/xrBeginSession before it permits a
     * reference space or swapchain to be created. */
    struct timespec pause = {.tv_nsec = 10000000};
    struct timespec started, now;
    clock_gettime(CLOCK_MONOTONIC, &started);
    while (!app.running) {
        poll_events(&app);
        clock_gettime(CLOCK_MONOTONIC, &now);
        need(now.tv_sec - started.tv_sec < 10,
             "OpenXR session did not enter READY within ten seconds");
        if (!app.running) nanosleep(&pause, NULL);
    }
    XrReferenceSpaceCreateInfo space_info = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL, .poseInReferenceSpace = {{0,0,0,1},{0,0,0}}};
    need_xr("xrCreateReferenceSpace", xrCreateReferenceSpace(app.session, &space_info, &app.space));
    initialize_swapchain(&app);
    create_render_resources(&app);
    clock_gettime(CLOCK_MONOTONIC, &started);
    for (;;) {
        poll_events(&app);
        if (app.running) render_frame(&app); else nanosleep(&pause, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - started.tv_sec >= 30) break;
    }
    printf("openxr_summary xr_frames=%" PRIu64 " rendered_frames=%" PRIu64 " session_changes=%" PRIu64 "\n", app.xr_frames, app.rendered_frames, app.session_changes);
    return 0;
}
