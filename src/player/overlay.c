#define _POSIX_C_SOURCE 200809L

#include "renderer.h"

#define OVERLAY_DEPTH_FORMAT VK_FORMAT_D16_UNORM
#define BEAM_HALF_WIDTH 0.0012f
#define BEAM_DOT_RADIUS 0.006f /* per metre of beam */
/* Overlay resolution against the recommended eye image, for the panel
 * text: about the panels' own pixel density (1.25 on the Frame looked soft
 * against quad layers; STEREONAUT_OVERLAY_SCALE overrides). */
#define OVERLAY_SCALE 1.75f

struct model_push {
    struct mat4 mvp;
    float normal_x[4], normal_y[4], normal_z[4];
    float color[4];
};

struct beam_push {
    struct mat4 view_projection;
    float start[4], end[4], eye[4], color[4];
};

_Static_assert(sizeof(struct model_push) <= 128, "model push too large");
_Static_assert(sizeof(struct beam_push) <= 128, "beam push too large");

static void make_buffer(struct app *app, VkDeviceSize size,
                        VkBufferUsageFlags usage, VkBuffer *buffer,
                        VkDeviceMemory *memory, void **mapping)
{
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    need_vk("vkCreateBuffer(overlay)",
            vkCreateBuffer(app->gpu.device, &info, NULL, buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(app->gpu.device, *buffer, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(
            app->gpu.physical, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    need_vk("vkAllocateMemory(overlay buffer)",
            vkAllocateMemory(app->gpu.device, &allocation, NULL, memory));
    need_vk("vkBindBufferMemory(overlay)",
            vkBindBufferMemory(app->gpu.device, *buffer, *memory, 0));
    need_vk("vkMapMemory(overlay)",
            vkMapMemory(app->gpu.device, *memory, 0, size, 0, mapping));
}

static VkPipeline overlay_pipeline(struct app *app, const char *vertex_path,
                                   const char *fragment_path,
                                   VkPipelineLayout layout, bool mesh)
{
    VkShaderModule vert = shader(app->gpu.device, vertex_path);
    VkShaderModule frag = shader(app->gpu.device, fragment_path);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag,
         .pName = "main"},
    };
    VkVertexInputBindingDescription binding = {
        .binding = 0, .stride = sizeof(struct gltf_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    VkVertexInputAttributeDescription attributes[3] = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
         .offset = offsetof(struct gltf_vertex, position)},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
         .offset = offsetof(struct gltf_vertex, normal)},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
         .offset = offsetof(struct gltf_vertex, uv)},
    };
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = mesh ? 1U : 0U,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = mesh ? 3U : 0U,
        .pVertexAttributeDescriptions = attributes,
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = mesh ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                         : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    VkViewport viewport = {.width = (float)app->overlay.width,
                           .height = (float)app->overlay.height, .maxDepth = 1};
    VkRect2D scissor = {.extent = {app->overlay.width, app->overlay.height}};
    VkPipelineViewportStateCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport,
        .scissorCount = 1, .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    /* The beam is tested against the controller but writes no depth. */
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = mesh,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .blendEnable = !mesh,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment,
    };
    VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
        .pInputAssemblyState = &assembly, .pViewportState = &view,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pDepthStencilState = &depth, .pColorBlendState = &blend,
        .layout = layout, .renderPass = app->overlay.pass,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    need_vk("vkCreateGraphicsPipelines(overlay)",
            vkCreateGraphicsPipelines(app->gpu.device, VK_NULL_HANDLE, 1, &info,
                                      NULL, &pipeline));
    vkDestroyShaderModule(app->gpu.device, vert, NULL);
    vkDestroyShaderModule(app->gpu.device, frag, NULL);
    return pipeline;
}

/* SteamVR on a PC shows nothing of a second projection layer (the screen
 * stayed black): there the overlay is drawn into the eye images, at their
 * resolution, after the video. The Frame (with the Iris decoder) composites
 * the sharper overlay layer. */
bool overlay_in_eye_images(void)
{
    return access(DECODED_VIDEO_IRIS_DEVICE, F_OK) != 0;
}

void create_overlay(struct app *app)
{
    float scale = OVERLAY_SCALE;
    const char *forced = getenv("STEREONAUT_OVERLAY_SCALE");
    if (forced && atof(forced) >= 0.5 && atof(forced) <= 2.0)
        scale = (float)atof(forced);
    XrViewConfigurationView limits[2] = {
        {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW},
        {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW},
    };
    uint32_t limit_count = 0;
    need_xr("xrEnumerateViewConfigurationViews(overlay)",
            xrEnumerateViewConfigurationViews(
                app->xr.instance, app->xr.system,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &limit_count,
                limits));
    app->overlay.in_eye_images = overlay_in_eye_images();
    if (app->overlay.in_eye_images)
        scale = 1.0f;
    app->overlay.width = (uint32_t)((float)app->xr.width * scale + 0.5f);
    app->overlay.height = (uint32_t)((float)app->xr.height * scale + 0.5f);
    if (app->overlay.width > limits[0].maxImageRectWidth)
        app->overlay.width = limits[0].maxImageRectWidth;
    if (app->overlay.height > limits[0].maxImageRectHeight)
        app->overlay.height = limits[0].maxImageRectHeight;
    for (uint32_t eye = 0; eye < 2 && app->overlay.in_eye_images; ++eye)
        app->overlay.image_counts[eye] = app->xr.image_count;
    for (uint32_t eye = 0; eye < 2 && !app->overlay.in_eye_images; ++eye) {
        XrSwapchainCreateInfo swapchain = {
            .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
            .format = app->xr.color_format, .sampleCount = 1,
            .width = app->overlay.width, .height = app->overlay.height, .faceCount = 1,
            .arraySize = 1, .mipCount = 1,
        };
        need_xr("xrCreateSwapchain(overlay)",
                xrCreateSwapchain(app->xr.session, &swapchain,
                                  &app->overlay.swapchains[eye]));
        uint32_t *count = &app->overlay.image_counts[eye];
        need_xr("xrEnumerateSwapchainImages(overlay count)",
                xrEnumerateSwapchainImages(app->overlay.swapchains[eye], 0,
                                           count, NULL));
        need(*count > 0 && *count <= MAX_IMAGES,
             "unexpected overlay swapchain image count");
        for (uint32_t i = 0; i < *count; ++i)
            app->overlay.images[eye][i].type =
                XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        need_xr("xrEnumerateSwapchainImages(overlay list)",
                xrEnumerateSwapchainImages(
                    app->overlay.swapchains[eye], *count, count,
                    (XrSwapchainImageBaseHeader *)app->overlay.images[eye]));
    }

    VkImageCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = OVERLAY_DEPTH_FORMAT,
        .extent = {app->overlay.width, app->overlay.height, 1}, .mipLevels = 1,
        .arrayLayers = 2, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    need_vk("vkCreateImage(overlay depth)",
            vkCreateImage(app->gpu.device, &depth, NULL, &app->overlay.depth));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(app->gpu.device, app->overlay.depth,
                                 &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(app->gpu.physical,
                                       requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    need_vk("vkAllocateMemory(overlay depth)",
            vkAllocateMemory(app->gpu.device, &allocation, NULL,
                             &app->overlay.depth_memory));
    need_vk("vkBindImageMemory(overlay depth)",
            vkBindImageMemory(app->gpu.device, app->overlay.depth,
                              app->overlay.depth_memory, 0));

    /* Into the eye images the video drawn before stays (load). */
    VkAttachmentDescription attachments[2] = {
        {.format = app->xr.color_format, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = app->overlay.in_eye_images ? VK_ATTACHMENT_LOAD_OP_LOAD
                                              : VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = app->overlay.in_eye_images
                              ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                              : VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.format = OVERLAY_DEPTH_FORMAT, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
    };
    VkAttachmentReference color_reference = {
        .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference depth_reference = {
        .attachment = 1,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color_reference,
        .pDepthStencilAttachment = &depth_reference,
    };
    /* The two eye passes share the depth image's layers; earlier passes
     * must have finished with it. */
    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = attachments,
        .subpassCount = 1, .pSubpasses = &subpass,
        .dependencyCount = 1, .pDependencies = &dependency,
    };
    need_vk("vkCreateRenderPass(overlay)",
            vkCreateRenderPass(app->gpu.device, &pass, NULL, &app->overlay.pass));
    for (uint32_t eye = 0; eye < 2; ++eye) {
        VkImageViewCreateInfo view = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = app->overlay.depth, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = OVERLAY_DEPTH_FORMAT,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                 .levelCount = 1, .baseArrayLayer = eye,
                                 .layerCount = 1},
        };
        need_vk("vkCreateImageView(overlay depth)",
                vkCreateImageView(app->gpu.device, &view, NULL,
                                  &app->overlay.depth_views[eye]));
    }
    for (uint32_t eye = 0; eye < 2; ++eye)
        for (uint32_t image = 0; image < app->overlay.image_counts[eye];
             ++image) {
            if (app->overlay.in_eye_images) {
                VkImageView views[2] = {app->render.image_views[image][eye],
                                        app->overlay.depth_views[eye]};
                VkFramebufferCreateInfo framebuffer = {
                    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                    .renderPass = app->overlay.pass, .attachmentCount = 2,
                    .pAttachments = views, .width = app->overlay.width,
                    .height = app->overlay.height, .layers = 1,
                };
                need_vk("vkCreateFramebuffer(overlay in eye image)",
                        vkCreateFramebuffer(
                            app->gpu.device, &framebuffer, NULL,
                            &app->overlay.framebuffers[eye][image]));
                continue;
            }
            VkImageViewCreateInfo view = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = app->overlay.images[eye][image].image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = app->xr.color_format,
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                     .levelCount = 1, .layerCount = 1},
            };
            need_vk("vkCreateImageView(overlay)",
                    vkCreateImageView(app->gpu.device, &view, NULL,
                                      &app->overlay.views[eye][image]));
            VkImageView views[2] = {app->overlay.views[eye][image],
                                    app->overlay.depth_views[eye]};
            VkFramebufferCreateInfo framebuffer = {
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = app->overlay.pass, .attachmentCount = 2,
                .pAttachments = views, .width = app->overlay.width,
                .height = app->overlay.height, .layers = 1,
            };
            need_vk("vkCreateFramebuffer(overlay)",
                    vkCreateFramebuffer(app->gpu.device, &framebuffer, NULL,
                                        &app->overlay.framebuffers[eye][image]));
        }

    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    need_vk("vkCreateSampler(model)",
            vkCreateSampler(app->gpu.device, &sampler, NULL,
                            &app->overlay.model_sampler));
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = &app->overlay.model_sampler,
    };
    VkDescriptorSetLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding,
    };
    need_vk("vkCreateDescriptorSetLayout(model)",
            vkCreateDescriptorSetLayout(app->gpu.device, &layout, NULL,
                                        &app->overlay.model_descriptor_layout));
    VkDescriptorPoolSize size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = RENDER_MODELS_MAX,
    };
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = RENDER_MODELS_MAX, .poolSizeCount = 1,
        .pPoolSizes = &size,
    };
    need_vk("vkCreateDescriptorPool(model)",
            vkCreateDescriptorPool(app->gpu.device, &pool, NULL,
                                   &app->overlay.model_descriptor_pool));
    VkPushConstantRange model_push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .size = sizeof(struct model_push),
    };
    VkPipelineLayoutCreateInfo model_layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &app->overlay.model_descriptor_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &model_push,
    };
    need_vk("vkCreatePipelineLayout(model)",
            vkCreatePipelineLayout(app->gpu.device, &model_layout, NULL,
                                   &app->overlay.model_pipeline_layout));
    VkPushConstantRange beam_push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .size = sizeof(struct beam_push),
    };
    VkPipelineLayoutCreateInfo beam_layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &beam_push,
    };
    need_vk("vkCreatePipelineLayout(beam)",
            vkCreatePipelineLayout(app->gpu.device, &beam_layout, NULL,
                                   &app->overlay.beam_pipeline_layout));
    app->overlay.model_pipeline = overlay_pipeline(
        app, "controller-model.vert.spv", "controller-model.frag.spv",
        app->overlay.model_pipeline_layout, true);
    app->overlay.beam_pipeline = overlay_pipeline(app, "laser.vert.spv",
                                          "laser.frag.spv",
                                          app->overlay.beam_pipeline_layout, false);
    printf("overlay images=%u+%u dimensions=%ux%u maximum=%ux%u depth=%d"
           " drawn_into=%s\n",
           app->overlay.image_counts[0], app->overlay.image_counts[1],
           app->overlay.width, app->overlay.height,
           limits[0].maxImageRectWidth, limits[0].maxImageRectHeight,
           OVERLAY_DEPTH_FORMAT,
           app->overlay.in_eye_images ? "eye_images" : "layer");
}

static void destroy_gpu_model(struct app *app, struct gpu_model *model)
{
    if (model->texture_view)
        vkDestroyImageView(app->gpu.device, model->texture_view, NULL);
    if (model->texture)
        vkDestroyImage(app->gpu.device, model->texture, NULL);
    if (model->texture_memory)
        vkFreeMemory(app->gpu.device, model->texture_memory, NULL);
    if (model->vertices)
        vkDestroyBuffer(app->gpu.device, model->vertices, NULL);
    if (model->vertex_memory)
        vkFreeMemory(app->gpu.device, model->vertex_memory, NULL);
    if (model->indices)
        vkDestroyBuffer(app->gpu.device, model->indices, NULL);
    if (model->index_memory)
        vkFreeMemory(app->gpu.device, model->index_memory, NULL);
    free(model->animation);
    gltf_model_free(&model->mesh);
    *model = (struct gpu_model){0};
}

/* The model's first texture with its mip chain (all parts share it). */
static bool upload_model_texture(struct app *app, struct gpu_model *model)
{
    const struct gltf_texture *texture = &model->mesh.textures[0];
    uint32_t levels = 0;
    for (uint32_t size = texture->width > texture->height ? texture->width
                                                          : texture->height;
         size; size >>= 1)
        ++levels;
    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
        .extent = {texture->width, texture->height, 1}, .mipLevels = levels,
        .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    need_vk("vkCreateImage(model texture)",
            vkCreateImage(app->gpu.device, &image, NULL, &model->texture));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(app->gpu.device, model->texture, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(app->gpu.physical,
                                       requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    need_vk("vkAllocateMemory(model texture)",
            vkAllocateMemory(app->gpu.device, &allocation, NULL,
                             &model->texture_memory));
    need_vk("vkBindImageMemory(model texture)",
            vkBindImageMemory(app->gpu.device, model->texture,
                              model->texture_memory, 0));
    VkDeviceSize bytes = (VkDeviceSize)texture->width * texture->height * 4U;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    void *mapping;
    make_buffer(app, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging,
                &staging_memory, &mapping);
    memcpy(mapping, texture->rgba, (size_t)bytes);
    VkCommandBufferAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->render.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer command;
    need_vk("vkAllocateCommandBuffers(model)",
            vkAllocateCommandBuffers(app->gpu.device, &allocate, &command));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    need_vk("vkBeginCommandBuffer(model)",
            vkBeginCommandBuffer(command, &begin));
    struct ui_texture as_panel = {
        .width = texture->width, .height = texture->height,
        .mip_levels = levels, .image = model->texture,
    };
    record_ui_upload(command, &as_panel, VK_NULL_HANDLE, staging, false);
    need_vk("vkEndCommandBuffer(model)", vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    need_vk("vkCreateFence(model)",
            vkCreateFence(app->gpu.device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command,
    };
    need_vk("vkQueueSubmit(model)",
            vkQueueSubmit(app->gpu.queue, 1, &submit, fence));
    need_vk("vkWaitForFences(model)",
            vkWaitForFences(app->gpu.device, 1, &fence, VK_TRUE,
                            UINT64_C(5000000000)));
    vkDestroyFence(app->gpu.device, fence, NULL);
    vkFreeCommandBuffers(app->gpu.device, app->render.command_pool, 1, &command);
    vkDestroyBuffer(app->gpu.device, staging, NULL);
    vkFreeMemory(app->gpu.device, staging_memory, NULL);
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = model->texture, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = levels, .layerCount = 1},
    };
    need_vk("vkCreateImageView(model texture)",
            vkCreateImageView(app->gpu.device, &view, NULL,
                              &model->texture_view));
    VkDescriptorSetAllocateInfo set = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = app->overlay.model_descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &app->overlay.model_descriptor_layout,
    };
    need_vk("vkAllocateDescriptorSets(model)",
            vkAllocateDescriptorSets(app->gpu.device, &set, &model->set));
    VkDescriptorImageInfo descriptor = {
        .imageView = model->texture_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = model->set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &descriptor,
    };
    vkUpdateDescriptorSets(app->gpu.device, 1, &write, 0, NULL);
    return true;
}

/* Turns the runtime's assets (render-models.c) into buffers and
 * textures; replaces the previous ones. */
void upload_render_models(struct app *app)
{
    need_vk("vkDeviceWaitIdle(models)", vkDeviceWaitIdle(app->gpu.device));
    for (uint32_t i = 0; i < app->overlay.gpu_model_count; ++i)
        destroy_gpu_model(app, &app->overlay.gpu_models[i]);
    app->overlay.gpu_model_count = 0;
    need_vk("vkResetDescriptorPool(models)",
            vkResetDescriptorPool(app->gpu.device, app->overlay.model_descriptor_pool, 0));
    for (uint32_t i = 0; i < app->overlay.render_models.count; ++i) {
        const struct render_model *source = &app->overlay.render_models.models[i];
        struct gpu_model *model = &app->overlay.gpu_models[app->overlay.gpu_model_count];
        char error[128] = "";
        if (gltf_model_load(source->glb, source->glb_size, &model->mesh,
                            error, sizeof(error)) < 0 ||
            !model->mesh.texture_count || !model->mesh.index_count) {
            fprintf(stderr, "render_model %u not shown: %s\n", i,
                    error[0] ? error : "no texture or triangles");
            gltf_model_free(&model->mesh);
            continue;
        }
        void *mapping;
        make_buffer(app,
                    (VkDeviceSize)model->mesh.vertex_count *
                        sizeof(struct gltf_vertex),
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &model->vertices,
                    &model->vertex_memory, &mapping);
        memcpy(mapping, model->mesh.vertices,
               model->mesh.vertex_count * sizeof(struct gltf_vertex));
        make_buffer(app,
                    (VkDeviceSize)model->mesh.index_count * sizeof(uint32_t),
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &model->indices,
                    &model->index_memory, &mapping);
        memcpy(mapping, model->mesh.indices,
               model->mesh.index_count * sizeof(uint32_t));
        upload_model_texture(app, model);
        /* glTF nodes the runtime animates, by name. */
        model->animation = malloc(model->mesh.node_count * sizeof(int));
        need(model->animation != NULL, "allocate model animation");
        for (uint32_t n = 0; n < model->mesh.node_count; ++n) {
            model->animation[n] = -1;
            for (uint32_t a = 0; a < source->node_count; ++a)
                if (strcmp(model->mesh.nodes[n].name,
                           source->node_names[a]) == 0)
                    model->animation[n] = (int)a;
        }
        model->source = i;
        app->overlay.gpu_model_count++;
        printf("render_model_upload index=%u triangles=%u nodes=%u\n", i,
               model->mesh.index_count / 3, model->mesh.node_count);
    }
}

/* Records the overlay's two eye passes into `command`. */
void record_overlay(struct app *app, VkCommandBuffer command,
                    const uint32_t images[2], const XrView views[2],
                    XrTime time, const struct ui_texture *panel,
                    struct mat4 panel_matrix,
                    const float beam_lengths[2],
                    const bool beam_dots[2])
{
    struct mat4 node_world[64];
    struct mat4 model_world[RENDER_MODELS_MAX];
    bool model_valid[RENDER_MODELS_MAX] = {false};
    render_models_update(&app->overlay.render_models, time);
    for (uint32_t i = 0; i < app->overlay.gpu_model_count; ++i) {
        const struct render_model *source =
            &app->overlay.render_models.models[app->overlay.gpu_models[i].source];
        XrSpaceLocation location = {.type = XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags valid =
            XR_SPACE_LOCATION_POSITION_VALID_BIT |
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(source->space, app->xr.space, time,
                                       &location)) &&
            (location.locationFlags & valid) == valid) {
            model_world[i] = pose_matrix(location.pose);
            model_valid[i] = true;
        }
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        VkClearValue clears[2] = {
            {.color = {{0.0f, 0.0f, 0.0f, 0.0f}}},
            {.depthStencil = {1.0f, 0}},
        };
        VkRenderPassBeginInfo pass = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = app->overlay.pass,
            .framebuffer = app->overlay.framebuffers[eye][images[eye]],
            .renderArea = {.extent = {app->overlay.width, app->overlay.height}},
            .clearValueCount = 2, .pClearValues = clears,
        };
        vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        struct mat4 view_projection = multiply(
            projection_from_fov(views[eye].fov),
            view_from_pose(views[eye].pose));
        if (panel && panel->ready) {
            struct mat4 mvp = multiply(view_projection, panel_matrix);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              app->panels.panel_pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    app->panels.panel_pipeline_layout, 0, 1,
                                    &panel->descriptor_set, 0, NULL);
            vkCmdPushConstants(command, app->panels.panel_pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp),
                               &mvp);
            vkCmdDraw(command, 4, 1, 0, 0);
        }
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          app->overlay.model_pipeline);
        for (uint32_t i = 0; i < app->overlay.gpu_model_count; ++i) {
            struct gpu_model *model = &app->overlay.gpu_models[i];
            if (!model_valid[i] || model->mesh.node_count > 64)
                continue;
            const struct render_model *source =
                &app->overlay.render_models.models[model->source];
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(command, 0, 1, &model->vertices, &offset);
            vkCmdBindIndexBuffer(command, model->indices, 0,
                                 VK_INDEX_TYPE_UINT32);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    app->overlay.model_pipeline_layout, 0, 1,
                                    &model->set, 0, NULL);
            for (uint32_t n = 0; n < model->mesh.node_count; ++n) {
                const struct gltf_node *node = &model->mesh.nodes[n];
                int animated = model->animation[n];
                struct mat4 local;
                if (animated >= 0) {
                    if (!source->node_visible[animated]) {
                        node_world[n] = model_world[i];
                        continue;
                    }
                    local = pose_matrix(source->node_poses[animated]);
                } else {
                    memcpy(local.v, node->local, sizeof(local.v));
                }
                node_world[n] = multiply(node->parent >= 0
                                             ? node_world[node->parent]
                                             : model_world[i],
                                         local);
                if (animated >= 0 && !source->node_visible[animated])
                    continue;
                struct model_push push = {
                    .mvp = multiply(view_projection, node_world[n]),
                };
                for (int r = 0; r < 3; ++r) {
                    push.normal_x[r] = node_world[n].v[r];
                    push.normal_y[r] = node_world[n].v[4 + r];
                    push.normal_z[r] = node_world[n].v[8 + r];
                }
                for (uint32_t p = 0; p < node->primitive_count; ++p) {
                    const struct gltf_primitive *primitive =
                        &model->mesh.primitives[node->first_primitive + p];
                    memcpy(push.color, primitive->color, sizeof(push.color));
                    vkCmdPushConstants(command, app->overlay.model_pipeline_layout,
                                       VK_SHADER_STAGE_VERTEX_BIT |
                                           VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(push), &push);
                    vkCmdDrawIndexed(command, primitive->index_count, 1,
                                     primitive->first_index, 0, 0);
                }
            }
        }
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          app->overlay.beam_pipeline);
        for (unsigned hand = 0; hand < 2; ++hand) {
            if (!app->overlay.lasers[hand].valid)
                continue;
            XrPosef aim = app->overlay.lasers[hand].aim;
            XrVector3f direction =
                rotate_vector(aim.orientation, (XrVector3f){0, 0, -1});
            float length = beam_lengths[hand];
            XrVector3f end = {aim.position.x + direction.x * length,
                              aim.position.y + direction.y * length,
                              aim.position.z + direction.z * length};
            struct beam_push push = {
                .view_projection = view_projection,
                .start = {aim.position.x, aim.position.y, aim.position.z,
                          BEAM_HALF_WIDTH},
                .end = {end.x, end.y, end.z, 0.0f},
                .eye = {views[eye].pose.position.x,
                        views[eye].pose.position.y,
                        views[eye].pose.position.z, 0.0f},
                .color = {0.55f, 0.78f, 1.0f, 0.85f},
            };
            vkCmdPushConstants(command, app->overlay.beam_pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            vkCmdDraw(command, 4, 1, 0, 0);
            if (beam_dots[hand]) {
                push.start[3] = BEAM_DOT_RADIUS * fmaxf(length, 0.6f);
                push.end[3] = 1.0f;
                push.color[3] = 1.0f;
                vkCmdPushConstants(command, app->overlay.beam_pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT |
                                       VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(push), &push);
                vkCmdDraw(command, 4, 1, 0, 0);
            }
        }
        vkCmdEndRenderPass(command);
    }
}
