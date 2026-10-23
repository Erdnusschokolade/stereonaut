#define _POSIX_C_SOURCE 200809L

#include "renderer.h"

/* Weight of the luma difference against the chroma difference when
 * keying a picked colour (shading of the background stays transparent). */
#define KEY_LUMA_WEIGHT 0.4f

/* Shader levels (enum nv12_video_levels): black and white luma and the
 * chroma range in 8-bit code values. */
void video_levels_uniform(uint32_t levels, float out[4])
{
    static const float table[3][4] = {
        [NV12_LEVELS_NORMAL] = {16.0f, 235.0f, 224.0f, 0.0f},
        [NV12_LEVELS_DEEP] = {0.0f, 235.0f, 224.0f, 0.0f},
        [NV12_LEVELS_FULL] = {0.0f, 255.0f, 255.0f, 0.0f},
    };
    memcpy(out, table[levels <= NV12_LEVELS_FULL ? levels : 0],
           sizeof(table[0]));
}

static void create_pipeline(struct app *app)
{
    VkAttachmentDescription attachment = {.format = app->xr.color_format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference reference = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference};
    VkRenderPassCreateInfo render_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass};
    need_vk("vkCreateRenderPass", vkCreateRenderPass(app->gpu.device, &render_info, NULL, &app->render.render_pass));
    for (uint32_t image = 0; image < app->xr.image_count; ++image) for (uint32_t eye = 0; eye < 2; ++eye) {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = app->xr.images[image].image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = app->xr.color_format,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .baseArrayLayer = eye, .layerCount = 1}};
        need_vk("vkCreateImageView", vkCreateImageView(app->gpu.device, &view, NULL, &app->render.image_views[image][eye]));
        VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = app->render.render_pass, .attachmentCount = 1, .pAttachments = &app->render.image_views[image][eye], .width = app->xr.width, .height = app->xr.height, .layers = 1};
        need_vk("vkCreateFramebuffer", vkCreateFramebuffer(app->gpu.device, &fb, NULL, &app->render.framebuffers[image][eye]));
    }
    /* Binding 1 is the NV12 frame (immutable YCbCr sampler), 2 and 3 the
     * TP10 luma and chroma planes; each pipeline uses only its own. */
    /* Binding 4 is the autofocus result; the measurement (compute) reads
     * the luma through binding 1 or 2. */
    VkDescriptorSetLayoutBinding bindings[5] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                       VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT |
                       VK_SHADER_STAGE_COMPUTE_BIT,
         .pImmutableSamplers = &app->video.sampler},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT |
                       VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT |
                       VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    VkDescriptorSetLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = bindings};
    need_vk("vkCreateDescriptorSetLayout", vkCreateDescriptorSetLayout(app->gpu.device, &layout, NULL, &app->render.descriptor_layout));
    VkPushConstantRange disparity_push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .size = sizeof(struct disparity_push),
    };
    VkPipelineLayoutCreateInfo pipeline_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &app->render.descriptor_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &disparity_push};
    need_vk("vkCreatePipelineLayout", vkCreatePipelineLayout(app->gpu.device, &pipeline_layout, NULL, &app->render.pipeline_layout));
    VkShaderModule vert = shader(app->gpu.device, "video-projection.vert.spv"), frag = shader(app->gpu.device, "nv12-projection.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"}, {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    VkViewport viewport = {.width = (float)app->xr.width, .height = (float)app->xr.height, .minDepth = 0, .maxDepth = 1};
    VkRect2D scissor = {.extent = {app->xr.width, app->xr.height}};
    VkPipelineViewportStateCreateInfo view = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend_attachment};
    VkGraphicsPipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly, .pViewportState = &view, .pRasterizationState = &raster, .pMultisampleState = &multisample, .pColorBlendState = &blend, .layout = app->render.pipeline_layout, .renderPass = app->render.render_pass};
    need_vk("vkCreateGraphicsPipelines", vkCreateGraphicsPipelines(app->gpu.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &app->render.pipeline));
    VkShaderModule tp10_frag =
        shader(app->gpu.device, "tp10-projection.frag.spv");
    stages[1].module = tp10_frag;
    need_vk("vkCreateGraphicsPipelines(TP10)", vkCreateGraphicsPipelines(app->gpu.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &app->render.pipeline_tp10));
    vkDestroyShaderModule(app->gpu.device, vert, NULL); vkDestroyShaderModule(app->gpu.device, frag, NULL);
    vkDestroyShaderModule(app->gpu.device, tp10_frag, NULL);
    const char *const disparity_shaders[2] = {
        "stereo-disparity.comp.spv", "stereo-disparity-tp10.comp.spv",
    };
    VkPipeline *disparity_pipelines[2] = {
        &app->autofocus.disparity_pipeline,
        &app->autofocus.disparity_pipeline_tp10,
    };
    for (int i = 0; i < 2; ++i) {
        VkShaderModule module = shader(app->gpu.device, disparity_shaders[i]);
        VkComputePipelineCreateInfo compute = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = module, .pName = "main"},
            .layout = app->render.pipeline_layout,
        };
        need_vk("vkCreateComputePipelines(disparity)",
                vkCreateComputePipelines(app->gpu.device, VK_NULL_HANDLE, 1,
                                         &compute, NULL,
                                         disparity_pipelines[i]));
        vkDestroyShaderModule(app->gpu.device, module, NULL);
    }
}

void create_render_resources(struct app *app)
{
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = app->gpu.queue_family};
    need_vk("vkCreateCommandPool", vkCreateCommandPool(app->gpu.device, &pool, NULL, &app->render.command_pool));
    VkCommandBufferAllocateInfo commands = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->render.command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    need_vk("vkAllocateCommandBuffers", vkAllocateCommandBuffers(app->gpu.device, &commands, &app->render.command));
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    need_vk("vkCreateFence", vkCreateFence(app->gpu.device, &fence, NULL, &app->render.fence));
    create_pipeline(app);
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 2 * UBO_STRIDE, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    need_vk("vkCreateBuffer(uniform)", vkCreateBuffer(app->gpu.device, &buffer, NULL, &app->render.uniform_buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(app->gpu.device, app->render.uniform_buffer, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(app->gpu.physical, requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    need_vk("vkAllocateMemory(uniform)", vkAllocateMemory(app->gpu.device, &allocation, NULL, &app->render.uniform_memory));
    need_vk("vkBindBufferMemory(uniform)", vkBindBufferMemory(app->gpu.device, app->render.uniform_buffer, app->render.uniform_memory, 0));
    need_vk("vkMapMemory(uniform)", vkMapMemory(app->gpu.device, app->render.uniform_memory, 0, VK_WHOLE_SIZE, 0, &app->render.uniform_mapping));
    VkBufferCreateInfo disparity = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(struct disparity_result),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    need_vk("vkCreateBuffer(disparity)", vkCreateBuffer(app->gpu.device, &disparity, NULL, &app->autofocus.disparity_buffer));
    vkGetBufferMemoryRequirements(app->gpu.device, app->autofocus.disparity_buffer, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type(app->gpu.physical, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    need_vk("vkAllocateMemory(disparity)", vkAllocateMemory(app->gpu.device, &allocation, NULL, &app->autofocus.disparity_memory));
    need_vk("vkBindBufferMemory(disparity)", vkBindBufferMemory(app->gpu.device, app->autofocus.disparity_buffer, app->autofocus.disparity_memory, 0));
    need_vk("vkMapMemory(disparity)", vkMapMemory(app->gpu.device, app->autofocus.disparity_memory, 0, VK_WHOLE_SIZE, 0, &app->autofocus.disparity_mapping));
    /* A set for every possible buffer (the count varies per stream) plus
     * one extra, which keeps a frame on screen across a session restart
     * while the regular buffers are imported again. */
    const uint32_t set_count = NV12_IPC_MAX_BUFFERS + 1U;
    VkDescriptorPoolSize sizes[3] = {
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         .descriptorCount = set_count},
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 3U * set_count},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = set_count},
    };
    VkDescriptorPoolCreateInfo descriptor_pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = set_count, .poolSizeCount = 3,
        .pPoolSizes = sizes};
    need_vk("vkCreateDescriptorPool", vkCreateDescriptorPool(app->gpu.device, &descriptor_pool, NULL, &app->render.descriptor_pool));
    VkDescriptorSetLayout layouts[NV12_IPC_MAX_BUFFERS + 1U];
    VkDescriptorSet sets[NV12_IPC_MAX_BUFFERS + 1U];
    for (uint32_t i = 0; i < set_count; ++i)
        layouts[i] = app->render.descriptor_layout;
    VkDescriptorSetAllocateInfo descriptor_allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = app->render.descriptor_pool,
        .descriptorSetCount = set_count,
        .pSetLayouts = layouts};
    need_vk("vkAllocateDescriptorSets",
            vkAllocateDescriptorSets(app->gpu.device, &descriptor_allocate,
                                     sets));
    for (uint32_t i = 0; i < NV12_IPC_MAX_BUFFERS; ++i)
        app->video.buffers[i].descriptor_set = sets[i];
    app->video.spare_set = sets[NV12_IPC_MAX_BUFFERS];
}

/* Passthrough shows the room where nothing is drawn, if the viewer chose
 * it and the runtime blends with the room (the Frame since SteamVR's
 * update of 2026-10-10; not the PC streaming to it). */
bool passthrough_active(const struct app *app)
{
    return app->passthrough && app->xr.alpha_blend_supported;
}

/* Premultiplied colour where there is no video (clear colour, outside the
 * 180 degree image, around the flat screen, keyed out): the usual near
 * black, or with passthrough black at the dimming's opacity over the
 * room. */
void background_color(const struct app *app, float color[4])
{
    if (passthrough_active(app)) {
        color[0] = color[1] = color[2] = 0.0f;
        color[3] = app->adjust[PLAYER_ADJUST_ENVIRONMENT_DIM];
    } else {
        color[0] = 0.005f;
        color[1] = 0.005f;
        color[2] = 0.008f;
        color[3] = 1.0f;
    }
}

/* Chroma key of the title for the video shaders (chroma_key in
 * nv12-projection.frag): green and blue as directions in the Cb/Cr plane
 * (BT.709, normalized chroma), a picked colour as chroma and luma;
 * threshold, softness and spill removal. */
void chroma_key_uniform(const struct app *app, float key[4],
                        float spill[4])
{
    static const float directions[3][2] = {
        {0.0f, 0.0f},
        {-0.3854f, -0.4542f}, /* green (0, 1, 0) */
        {0.5f, -0.0458f},     /* blue (0, 0, 1) */
    };
    int color = (int)(app->adjust[PLAYER_ADJUST_KEY_COLOR] + 0.5f);
    if (color < 0 || color > 3)
        color = 0;
    key[2] = app->adjust[PLAYER_ADJUST_KEY_THRESHOLD];
    key[3] = app->adjust[PLAYER_ADJUST_KEY_SOFTNESS];
    spill[0] = app->adjust[PLAYER_ADJUST_KEY_SPILL];
    spill[2] = spill[3] = 0.0f;
    if (color == 3) {
        float levels[4];
        video_levels_uniform(app->video_levels, levels);
        key[0] = (app->adjust[PLAYER_ADJUST_KEY_CB] - 128.0f) / levels[2];
        key[1] = (app->adjust[PLAYER_ADJUST_KEY_CR] - 128.0f) / levels[2];
        spill[1] = 2.0f;
        spill[2] = (app->adjust[PLAYER_ADJUST_KEY_Y] - levels[0]) /
                   (levels[1] - levels[0]);
        spill[3] = KEY_LUMA_WEIGHT;
        return;
    }
    float length = sqrtf(directions[color][0] * directions[color][0] +
                         directions[color][1] * directions[color][1]);
    key[0] = length > 0.0f ? directions[color][0] / length : 0.0f;
    key[1] = length > 0.0f ? directions[color][1] / length : 0.0f;
    spill[1] = color ? 1.0f : 0.0f;
}

/* Where a ray (LOCAL space, unit direction) hits the video as render_frame
 * shows it: texture uv in the picture the left eye sees and the distance
 * along the ray (if `distance` is not NULL), false if it misses. The
 * dome is the unit sphere around the eye position in dome axes, the flat
 * screen the quad of screen_model; mirrors nv12-projection.frag without
 * the stereo offsets. */
bool video_uv_at_ray(const struct app *app, XrVector3f origin,
                     XrVector3f direction, XrVector3f head,
                     struct mat4 world_to_dome,
                     XrVector3f eye_position, struct mat4 screen_model,
                     float uv[2], float *distance)
{
    float along = 0.0f;
    float u, v;
    if (app->projection_mode == NV12_PROJECTION_FLAT) {
        XrVector3f axis_x = {screen_model.v[0], screen_model.v[1],
                             screen_model.v[2]};
        XrVector3f axis_y = {screen_model.v[4], screen_model.v[5],
                             screen_model.v[6]};
        XrVector3f normal = {screen_model.v[8], screen_model.v[9],
                             screen_model.v[10]};
        XrVector3f centre = {screen_model.v[12], screen_model.v[13],
                             screen_model.v[14]};
        float facing = vector_dot(direction, normal);
        if (fabsf(facing) < 1e-6f)
            return false;
        XrVector3f to_centre = {centre.x - origin.x, centre.y - origin.y,
                                centre.z - origin.z};
        float t = vector_dot(to_centre, normal) / facing;
        if (t <= 0.0f)
            return false;
        along = t;
        XrVector3f offset = {origin.x + t * direction.x - centre.x,
                             origin.y + t * direction.y - centre.y,
                             origin.z + t * direction.z - centre.z};
        u = vector_dot(offset, axis_x) / vector_dot(axis_x, axis_x) + 0.5f;
        v = 0.5f - vector_dot(offset, axis_y) / vector_dot(axis_y, axis_y);
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
            return false;
    } else {
        XrVector3f from_head = {origin.x - head.x, origin.y - head.y,
                                origin.z - head.z};
        XrVector3f c = transform_direction(world_to_dome, from_head);
        c.x += eye_position.x;
        c.y += eye_position.y;
        c.z += eye_position.z;
        XrVector3f d = transform_direction(world_to_dome, direction);
        float length = sqrtf(vector_dot(d, d));
        if (length <= 0.0f || vector_dot(c, c) >= 1.0f)
            return false;
        d.x /= length;
        d.y /= length;
        d.z /= length;
        float b = vector_dot(c, d);
        float t = -b + sqrtf(b * b - vector_dot(c, c) + 1.0f);
        along = t / length;
        XrVector3f q = {c.x + t * d.x, c.y + t * d.y, c.z + t * d.z};
        float longitude = atan2f(q.x, -q.z);
        float latitude = asinf(fmaxf(-1.0f, fminf(1.0f, q.y)));
        if (app->projection_mode == NV12_PROJECTION_FISHEYE) {
            float half_angle = 0.5f * (float)app->fisheye_fov * PI_F / 180.0f;
            float radius = acosf(fmaxf(-1.0f, fminf(1.0f, -q.z))) /
                           half_angle;
            if (radius > 1.0f)
                return false;
            float planar = sqrtf(q.x * q.x + q.y * q.y);
            float ax = planar > 1e-6f ? q.x / planar : 0.0f;
            float ay = planar > 1e-6f ? q.y / planar : 0.0f;
            u = 0.5f + 0.5f * radius * ax;
            v = 0.5f - 0.5f * radius * ay;
        } else if (app->projection_mode == NV12_PROJECTION_EQUIRECT_180) {
            if (fabsf(longitude) > 0.5f * PI_F)
                return false;
            u = longitude / PI_F + 0.5f;
            v = 0.5f - latitude / PI_F;
        } else {
            u = longitude / (2.0f * PI_F) + 0.5f;
            u -= floorf(u);
            v = 0.5f - latitude / PI_F;
        }
    }
    float eye = app->swap_eyes ? 1.0f : 0.0f;
    if (app->stereo_layout == NV12_STEREO_SBS)
        u = 0.5f * eye + 0.5f * u;
    else if (app->stereo_layout == NV12_STEREO_TB)
        v = 0.5f * eye + 0.5f * v;
    uv[0] = u;
    uv[1] = v;
    if (distance)
        *distance = along;
    return true;
}

XrEnvironmentBlendMode environment_blend_mode(const struct app *app)
{
    return passthrough_active(app) ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                                   : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
}

/* Where the video lies: the dome around the head (moved by head motion,
 * zoom, height and slide) and the flat screen. */
void place_video(struct app *app, struct frame *f)
{
    /* Dome placement: centred on the head of the first frame (or of
     * "zentrieren"), turned by the gaze yaw and the viewer's rotation.
     * world_to_dome undoes yaw, then tilt, then roll. */
    if (app->recenter_pending &&
        (f->center.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        app->dome_center = f->center.pose.position;
        if (app->recenter_yaw) {
            set_home(app, f->center.pose, false);
            app->dome_yaw = -app->home_yaw;
        }
        app->recenter_pending = false;
        printf("player_recenter x=%.3f y=%.3f z=%.3f yaw=%.1f\n",
               app->dome_center.x, app->dome_center.y, app->dome_center.z,
               app->dome_yaw * 180.0f / PI_F);
    }
    const float degrees = PI_F / 180.0f;
    float yaw = app->dome_yaw + app->adjust[PLAYER_ADJUST_YAW] * degrees;
    float tilt = app->adjust[PLAYER_ADJUST_TILT] * degrees;
    float roll = app->adjust[PLAYER_ADJUST_ROLL] * degrees;
    f->world_to_dome = multiply(
        rotation_axis(2, roll),
        multiply(rotation_axis(0, -tilt), rotation_axis(1, yaw)));
    f->dome_to_world = multiply(
        rotation_axis(1, -yaw),
        multiply(rotation_axis(0, tilt), rotation_axis(2, -roll)));
    /* Eye inside the dome (radius 1 m): head motion, then zoom, height and
     * slide along the dome axes. */
    float motion = app->adjust[PLAYER_ADJUST_HEAD_MOTION];
    XrVector3f head = {
        (f->center.pose.position.x - app->dome_center.x) * motion,
        (f->center.pose.position.y - app->dome_center.y) * motion,
        (f->center.pose.position.z - app->dome_center.z) * motion,
    };
    f->eye_position = transform_direction(f->world_to_dome, head);
    f->eye_position.x -= app->adjust[PLAYER_ADJUST_SLIDE];
    f->eye_position.y -= app->adjust[PLAYER_ADJUST_HEIGHT];
    f->eye_position.z -= app->adjust[PLAYER_ADJUST_ZOOM];
    float eye_distance = sqrtf(vector_dot(f->eye_position, f->eye_position));
    if (eye_distance > 0.9f) {
        f->eye_position.x *= 0.9f / eye_distance;
        f->eye_position.y *= 0.9f / eye_distance;
        f->eye_position.z *= 0.9f / eye_distance;
    }
    /* Flat screen: 2 m wide, 2 m ahead of the dome centre, moved and
     * turned like the dome. */
    float eye_width = (float)app->video.width /
                      (app->stereo_layout == NV12_STEREO_SBS ? 2.0f : 1.0f);
    float eye_height = (float)app->video.height /
                       (app->stereo_layout == NV12_STEREO_TB ? 2.0f : 1.0f);
    float screen_height = eye_width > 0.0f && eye_height > 0.0f
                              ? 2.0f * eye_height / eye_width : 1.0f;
    XrVector3f screen = transform_direction(
        f->dome_to_world,
        (XrVector3f){app->adjust[PLAYER_ADJUST_SLIDE],
                     app->adjust[PLAYER_ADJUST_HEIGHT],
                     -(2.0f - app->adjust[PLAYER_ADJUST_ZOOM])});
    f->screen_model = f->dome_to_world;
    f->screen_model.v[12] = app->dome_center.x + screen.x;
    f->screen_model.v[13] = app->dome_center.y + screen.y;
    f->screen_model.v[14] = app->dome_center.z + screen.z;
    struct mat4 screen_scale = {{2.0f, 0, 0, 0, 0, screen_height, 0, 0,
                                 0, 0, 1, 0, 0, 0, 0, 1}};
    f->screen_model = multiply(f->screen_model, screen_scale);
}

void write_view_uniforms(struct app *app, struct frame *f)
{
    const float degrees = PI_F / 180.0f;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        struct mat4 projection = projection_from_fov(f->views[eye].fov);
        struct mat4 view = view_from_pose(f->views[eye].pose);
        float side = eye == 0 ? -1.0f : 1.0f;
        float stereo_x = (app->adjust[PLAYER_ADJUST_STEREO_X] +
                          app->autofocus.offset) * 0.5f;
        float stereo_y = app->adjust[PLAYER_ADJUST_STEREO_Y] * 0.5f;
        struct view_uniform uniform = {
            .mvp = multiply(projection, multiply(view, f->screen_model)),
            .camera_to_world = multiply(
                f->world_to_dome, rotation_from_pose(f->views[eye].pose)),
            /* w: half the fisheye lens angle (radians). */
            .eye_offset = {f->eye_position.x, f->eye_position.y,
                           f->eye_position.z,
                           0.5f * (float)app->fisheye_fov * degrees},
            .stereo_offset = {-side * stereo_x * degrees,
                              -side * stereo_y * degrees,
                              -side * stereo_x / 60.0f,
                              side * stereo_y / 60.0f},
            .picture = {app->adjust[PLAYER_ADJUST_BRIGHTNESS],
                        app->adjust[PLAYER_ADJUST_CONTRAST],
                        app->adjust[PLAYER_ADJUST_SATURATION],
                        app->adjust[PLAYER_ADJUST_GAMMA]},
            .fov_tangents =
                {
                    tanf(f->views[eye].fov.angleLeft),
                    tanf(f->views[eye].fov.angleRight),
                    tanf(f->views[eye].fov.angleDown),
                    tanf(f->views[eye].fov.angleUp),
                },
            .modes =
                {
                    (int32_t)app->projection_mode,
                    (int32_t)app->stereo_layout,
                    (int32_t)eye,
                    app->swap_eyes,
                },
            .source_info =
                {
                    (float)app->video.width,
                    (float)app->video.height,
                    app->video.width ? 1.0f / (float)app->video.width : 0.0f,
                    app->video.height ? 1.0f / (float)app->video.height : 0.0f,
                },
            .video_scale =
                {
                    app->video.coded_width ? (float)app->video.width /
                                                 (float)app->video.coded_width
                                           : 1.0f,
                    app->video.coded_height
                        ? (float)app->video.height /
                              (float)app->video.coded_height
                        : 1.0f,
                    0.0f,
                    0.0f,
                },
        };
        video_levels_uniform(app->video_levels, uniform.levels);
        background_color(app, uniform.background);
        chroma_key_uniform(app, uniform.chroma_key, uniform.chroma_spill);
        memcpy((uint8_t *)app->render.uniform_mapping + eye * UBO_STRIDE,
               &uniform, sizeof(uniform));
    }
}

/* The previous frame's commands are done (with the measurement). */
void wait_render_fence(struct app *app, struct frame *f)
{
    need_vk("vkWaitForFences(render)", vkWaitForFences(app->gpu.device, 1, &app->render.fence, VK_TRUE, UINT64_C(5000000000)));
    need_vk("vkResetFences(render)", vkResetFences(app->gpu.device, 1, &app->render.fence));
    f->fenced = monotonic_seconds();
}

/* Begins the frame's commands: the eye image and the video become
 * usable (a new host-memory picture is copied first). */
void record_video_start(struct app *app, struct frame *f)
{
    need_vk("vkResetCommandPool(render)", vkResetCommandPool(app->gpu.device, app->render.command_pool, 0));
    VkCommandBufferBeginInfo command_begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    need_vk("vkBeginCommandBuffer(render)", vkBeginCommandBuffer(app->render.command, &command_begin));
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image = app->xr.images[f->image_index].image, .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 2}};
    vkCmdPipelineBarrier(app->render.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    f->video_acquire = (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = app->gpu.queue_family,
        .image = f->video ? f->video->image : VK_NULL_HANDLE,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    if (f->video && f->video->host) {
        /* Own images, not shared with a foreign producer. */
        f->video_acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        f->video_acquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        if (f->video->producer_wait_pending) {
            record_host_upload(app, f->video);
            f->video->producer_wait_pending = false;
            f->copied_video = true;
        }
    }
    VkImageMemoryBarrier video_acquires[2] = {f->video_acquire,
                                              f->video_acquire};
    f->video_images = f->video && f->video->tp10 ? 2U : 1U;
    if (f->video_images == 2U)
        video_acquires[1].image = f->video->image_uv;
    if (f->video)
        vkCmdPipelineBarrier(
            app->render.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, NULL, 0, NULL, f->video_images, video_acquires);
}

/* The video into both eyes, then the video back to its producer. */
void record_video_passes(struct app *app, struct frame *f)
{
    for (uint32_t eye = 0; eye < 2; ++eye) {
        VkClearValue clear;
        background_color(app, clear.color.float32);
        VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = app->render.render_pass, .framebuffer = app->render.framebuffers[f->image_index][eye],
            .renderArea = {.extent = {app->xr.width, app->xr.height}}, .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(app->render.command, &pass,
                             VK_SUBPASS_CONTENTS_INLINE);
        if (f->video) {
            vkCmdBindPipeline(app->render.command,
                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                              f->video->tp10 ? app->render.pipeline_tp10
                                             : app->render.pipeline);
            uint32_t offset = eye * UBO_STRIDE;
            vkCmdBindDescriptorSets(app->render.command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    app->render.pipeline_layout, 0, 1,
                                    &f->video->descriptor_set, 1, &offset);
            vkCmdDraw(app->render.command, 4, 1, 0, 0);
        }
        vkCmdEndRenderPass(app->render.command);
    }
    VkImageMemoryBarrier video_release = f->video_acquire;
    video_release.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    video_release.dstAccessMask = 0;
    if (!f->video || !f->video->host) {
        video_release.srcQueueFamilyIndex = app->gpu.queue_family;
        video_release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    }
    VkImageMemoryBarrier video_releases[2] = {video_release, video_release};
    if (f->video_images == 2U)
        video_releases[1].image = f->video->image_uv;
    if (f->video)
        vkCmdPipelineBarrier(app->render.command,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                             0, NULL, f->video_images, video_releases);
}

/* Submits the frame's commands, after the decoder's fence for a new
 * DMA-BUF picture, and waits for them. */
void submit_frame(struct app *app, struct frame *f)
{
    need_vk("vkEndCommandBuffer(render)",
            vkEndCommandBuffer(app->render.command));
    VkPipelineStageFlags producer_stage =
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &app->render.command};
    if (f->video && f->video->producer_wait_pending) {
        struct dma_buf_export_sync_file sync_export = {
            .flags = DMA_BUF_SYNC_READ, .fd = -1,
        };
        if (ioctl(f->video->dma_buf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE,
                  &sync_export) < 0) {
            perror("DMA_BUF_IOCTL_EXPORT_SYNC_FILE");
            exit(2);
        }
        VkImportSemaphoreFdInfoKHR semaphore_import = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = f->video->producer_semaphore,
            .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = sync_export.fd,
        };
        need_vk(
            "vkImportSemaphoreFdKHR(producer)",
            app->gpu.import_semaphore_fd(app->gpu.device, &semaphore_import));
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &f->video->producer_semaphore;
        submit.pWaitDstStageMask = &producer_stage;
        f->video->producer_wait_pending = false;
    }
    f->recorded = monotonic_seconds();
    need_vk("vkQueueSubmit(render)", vkQueueSubmit(app->gpu.queue, 1, &submit, app->render.fence));
    f->submitted = monotonic_seconds();
    need_vk("vkWaitForFences(render completion)", vkWaitForFences(app->gpu.device, 1, &app->render.fence, VK_TRUE, UINT64_C(5000000000)));
    f->gpu_ms = (monotonic_seconds() - f->submitted) * 1e3;
}
