/* Continuously render decoded DMA-BUF frames (Iris, dav1d or a PC hardware
 * decoder) on the validated OpenXR quad. */
#define _POSIX_C_SOURCE 200809L

#include "renderer.h"

static uint64_t parse_u64(const char *text, const char *name)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || !end || *end || value == 0) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return (uint64_t)value;
}

/* ---- picture position and adjustments ------------------------------- */

/* Shader levels (enum nv12_video_levels): black and white luma and the
 * chroma range in 8-bit code values. */
static void video_levels_uniform(uint32_t levels, float out[4])
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
    need_vk("vkCreateRenderPass", vkCreateRenderPass(app->gpu.device, &render_info, NULL, &app->render_pass));
    for (uint32_t image = 0; image < app->xr.image_count; ++image) for (uint32_t eye = 0; eye < 2; ++eye) {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = app->xr.images[image].image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = app->xr.color_format,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .baseArrayLayer = eye, .layerCount = 1}};
        need_vk("vkCreateImageView", vkCreateImageView(app->gpu.device, &view, NULL, &app->image_views[image][eye]));
        VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = app->render_pass, .attachmentCount = 1, .pAttachments = &app->image_views[image][eye], .width = app->xr.width, .height = app->xr.height, .layers = 1};
        need_vk("vkCreateFramebuffer", vkCreateFramebuffer(app->gpu.device, &fb, NULL, &app->framebuffers[image][eye]));
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
    need_vk("vkCreateDescriptorSetLayout", vkCreateDescriptorSetLayout(app->gpu.device, &layout, NULL, &app->descriptor_layout));
    VkPushConstantRange disparity_push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .size = sizeof(struct disparity_push),
    };
    VkPipelineLayoutCreateInfo pipeline_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &app->descriptor_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &disparity_push};
    need_vk("vkCreatePipelineLayout", vkCreatePipelineLayout(app->gpu.device, &pipeline_layout, NULL, &app->pipeline_layout));
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
    VkGraphicsPipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly, .pViewportState = &view, .pRasterizationState = &raster, .pMultisampleState = &multisample, .pColorBlendState = &blend, .layout = app->pipeline_layout, .renderPass = app->render_pass};
    need_vk("vkCreateGraphicsPipelines", vkCreateGraphicsPipelines(app->gpu.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &app->pipeline));
    VkShaderModule tp10_frag = shader(app->gpu.device, "tp10-projection.frag.spv");
    stages[1].module = tp10_frag;
    need_vk("vkCreateGraphicsPipelines(TP10)", vkCreateGraphicsPipelines(app->gpu.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &app->pipeline_tp10));
    vkDestroyShaderModule(app->gpu.device, vert, NULL); vkDestroyShaderModule(app->gpu.device, frag, NULL);
    vkDestroyShaderModule(app->gpu.device, tp10_frag, NULL);
    const char *const disparity_shaders[2] = {
        "stereo-disparity.comp.spv", "stereo-disparity-tp10.comp.spv",
    };
    VkPipeline *disparity_pipelines[2] = {
        &app->autofocus.disparity_pipeline, &app->autofocus.disparity_pipeline_tp10,
    };
    for (int i = 0; i < 2; ++i) {
        VkShaderModule module = shader(app->gpu.device, disparity_shaders[i]);
        VkComputePipelineCreateInfo compute = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = module, .pName = "main"},
            .layout = app->pipeline_layout,
        };
        need_vk("vkCreateComputePipelines(disparity)",
                vkCreateComputePipelines(app->gpu.device, VK_NULL_HANDLE, 1,
                                         &compute, NULL,
                                         disparity_pipelines[i]));
        vkDestroyShaderModule(app->gpu.device, module, NULL);
    }
}

_Static_assert(TRICKPLAY_STRIP == PLAYER_UI_PREVIEW_STRIP,
               "trickplay strip and panel preview differ");

static void create_render_resources(struct app *app)
{
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = app->gpu.queue_family};
    need_vk("vkCreateCommandPool", vkCreateCommandPool(app->gpu.device, &pool, NULL, &app->command_pool));
    VkCommandBufferAllocateInfo commands = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    need_vk("vkAllocateCommandBuffers", vkAllocateCommandBuffers(app->gpu.device, &commands, &app->command));
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    need_vk("vkCreateFence", vkCreateFence(app->gpu.device, &fence, NULL, &app->fence));
    create_pipeline(app);
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 2 * UBO_STRIDE, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    need_vk("vkCreateBuffer(uniform)", vkCreateBuffer(app->gpu.device, &buffer, NULL, &app->uniform_buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(app->gpu.device, app->uniform_buffer, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(app->gpu.physical, requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    need_vk("vkAllocateMemory(uniform)", vkAllocateMemory(app->gpu.device, &allocation, NULL, &app->uniform_memory));
    need_vk("vkBindBufferMemory(uniform)", vkBindBufferMemory(app->gpu.device, app->uniform_buffer, app->uniform_memory, 0));
    need_vk("vkMapMemory(uniform)", vkMapMemory(app->gpu.device, app->uniform_memory, 0, VK_WHOLE_SIZE, 0, &app->uniform_mapping));
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
    need_vk("vkCreateDescriptorPool", vkCreateDescriptorPool(app->gpu.device, &descriptor_pool, NULL, &app->descriptor_pool));
    VkDescriptorSetLayout layouts[NV12_IPC_MAX_BUFFERS + 1U];
    VkDescriptorSet sets[NV12_IPC_MAX_BUFFERS + 1U];
    for (uint32_t i = 0; i < set_count; ++i)
        layouts[i] = app->descriptor_layout;
    VkDescriptorSetAllocateInfo descriptor_allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = app->descriptor_pool,
        .descriptorSetCount = set_count,
        .pSetLayouts = layouts};
    need_vk("vkAllocateDescriptorSets",
            vkAllocateDescriptorSets(app->gpu.device, &descriptor_allocate,
                                     sets));
    for (uint32_t i = 0; i < NV12_IPC_MAX_BUFFERS; ++i)
        app->video.buffers[i].descriptor_set = sets[i];
    app->video.spare_set = sets[NV12_IPC_MAX_BUFFERS];
}

/* ---- controller bindings ---------------------------------------------- */

/* Follows the update script; once an update is installed the launcher
 * restarts Stereonaut (not in the middle of a title). */
static void poll_updater(struct app *app)
{
    if (updater_poll(&app->updater))
        refresh_update_status(app);
    if (app->updater.state == UPDATER_DONE && !app->playing &&
        !app->awaiting_command && !app->quit) {
        printf("update installed; restarting\n");
        app->awaiting_command = true;
        send_control(app, PLAYER_UI_RESTART_REQUESTED, NULL, 0);
        app->quit = true;
    }
}


/* ---- launcher control socket ---------------------------------------- */

void send_control(struct app *app, uint32_t type, const char *path,
                  int64_t start_us)
{
    struct player_ui_result result = {.type = type, .start_us = start_us};
    if (path)
        snprintf(result.path, sizeof(result.path), "%s", path);
    ssize_t written;
    do {
        written = send(app->ui_fd, &result, sizeof(result), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written != (ssize_t)sizeof(result))
        app->quit = true; /* the launcher is gone */
}

/* Back to the browser: drop every video import and reset player state. */
static void end_playback(struct app *app)
{
    app->popup_open = false;
    video_import_pending(app, true);
    need_vk("vkDeviceWaitIdle(end playback)", vkDeviceWaitIdle(app->gpu.device));
    discard_held_frame(app);
    for (uint32_t i = 0; i < app->video.buffer_count; ++i)
        destroy_video_buffer(app, &app->video.buffers[i]);
    if (app->link.fd >= 0)
        close(app->link.fd);
    app->link.fd = -1;
    app->playing = false;
    app->link.eos_received = false;
    app->video.current = -1;
    app->video.presented = false;
    app->awaiting_frame = true;
    app->seek_pending = false;
    app->seek_scrubbing = false;
    app->seek_resume = false;
    app->seek_show_panel = false;
    app->seek_burst = 0;
    app->grab_hand = -1;
    app->scrub_hand = -1;
    app->controls.learn_action = -1;
    memset(app->controls.held, 0, sizeof(app->controls.held));
    memset(app->controls.combined, 0, sizeof(app->controls.combined));
    memset(app->controls.repeat, 0, sizeof(app->controls.repeat));
    app->controls.grip_toggle[0] = app->controls.grip_toggle[1] = false;
    app->ui_hover = 0;
    app->ui_pointer_visible = false;
    app->link.player_state = NV12_PLAYER_PLAYING;
    printf("openxr_summary xr_frames=%" PRIu64
           " rendered_frames=%" PRIu64 " received_frames=%" PRIu64
           " unique_video_frames=%" PRIu64
           " repeated_video_frames=%" PRIu64
           " hidden_video_frames=%" PRIu64
           " pipeline_skipped_frames=%" PRIu64
           " released_frames=%" PRIu64
           " session_changes=%" PRIu64 "\n",
           app->xr_frames, app->rendered_frames, app->received_frames,
           app->unique_video_frames, app->repeated_video_frames,
           app->hidden_video_frames, app->pipeline_skipped_frames,
           app->released_frames, app->session_changes);
    app->xr_frames = app->rendered_frames = app->received_frames = 0;
    app->unique_video_frames = app->repeated_video_frames = 0;
    app->hidden_video_frames = app->pipeline_skipped_frames = 0;
    app->released_frames = 0;
    app->link.position_us = 0;
    app->link.duration_us = 0;
    app->video.width = app->video.height = 0;
    /* The trigger that pressed "back" is usually still held: ignore it
     * until a release so the browser does not react to it (it could
     * otherwise land on "Beenden" and close the program). */
    app->input.select[0].suppressed = app->input.select[1].suppressed = true;
    trickplay_open(app->trickplay, NULL);
    app->details_valid = false;
    for (int i = 0; i < TRICKPLAY_STRIP; ++i) {
        ui_image_free(app->preview[i]);
        app->preview[i] = NULL;
    }
    app->preview_visible = false;
    app->ui_dirty = true;
    app->browser.dirty = true;
    printf("playback_ended\n");
}

/* `location` is what the session plays; "jf:item:ID[:PARENT]" has
 * trickplay previews. */
static void begin_playback(struct app *app, int fd, const char *location)
{
    if (app->playing)
        end_playback(app);
    snprintf(app->location, sizeof(app->location), "%s", location);
    app->link.title_finished = false;
    app->last_input_time = monotonic_seconds();
    app->auto_hidden = false;
    if (strncmp(location, "jf:item:", 8) == 0) {
        char item[64] = "";
        size_t length = strcspn(location + 8, ":");
        if (length < sizeof(item))
            memcpy(item, location + 8, length);
        item[length < sizeof(item) ? length : 0] = '\0';
        trickplay_open(app->trickplay, item);
    }
    if (location[0])
        title_settings_key(location, app->title_key, sizeof(app->title_key));
    else
        app->title_key[0] = '\0';
    load_adjust(app, false);
    /* Centre the dome on the head at the first rendered frame, turned
     * towards home (kept across titles until the viewer centres again). */
    app->recenter_pending = true;
    app->recenter_yaw = false;
    app->dome_yaw = -app->home_yaw;
    app->autofocus.offset = 0.0f;
    app->autofocus.speed = 0.0f;
    app->autofocus.time = 0.0;
    app->autofocus.focused = false;
    app->autofocus.focus_disparity = app->autofocus.distance = 0.0f;
    app->link.fd = fd;
    app->settings_open = false;
    app->key_picking = app->autofocus.key_pick_request = false;
    app->autofocus.key_sample_pending = false;
    app->playing = true;
    app->awaiting_command = false;
    app->link.eos_received = false;
    app->ui_dirty = true;
    /* see end_playback */
    app->input.select[0].suppressed = app->input.select[1].suppressed = true;
    printf("playback_attached\n");
}

static void handle_control(struct app *app)
{
    for (;;) {
        struct player_ui_command command = {0};
        char control[CMSG_SPACE(sizeof(int))] = {0};
        struct iovec iov = {.iov_base = &command, .iov_len = sizeof(command)};
        struct msghdr received = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = control, .msg_controllen = sizeof(control),
        };
        ssize_t bytes = recvmsg(app->ui_fd, &received, MSG_DONTWAIT);
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (bytes <= 0) {
            app->quit = true; /* the launcher closed the socket */
            return;
        }
        if ((size_t)bytes != sizeof(command))
            continue;
        if (command.type == PLAYER_UI_PLAY) {
            struct cmsghdr *header = CMSG_FIRSTHDR(&received);
            if (header && header->cmsg_level == SOL_SOCKET &&
                header->cmsg_type == SCM_RIGHTS &&
                header->cmsg_len == CMSG_LEN(sizeof(int))) {
                int fd;
                memcpy(&fd, CMSG_DATA(header), sizeof(fd));
                command.path[sizeof(command.path) - 1] = '\0';
                begin_playback(app, fd, command.path);
            }
        } else if (command.type == PLAYER_UI_BROWSE) {
            bool finished = app->playing && app->link.title_finished;
            if (app->playing)
                end_playback(app);
            command.path[sizeof(command.path) - 1] = '\0';
            command.message[sizeof(command.message) - 1] = '\0';
            /* A title that played to its end may lead straight into the
             * next one (or itself) without showing the browser. */
            char next[PLAYER_UI_PATH_MAX];
            bool follow = false;
            if (app->skip_path[0] && !command.message[0]) {
                /* "Next/previous video" ended the session. */
                snprintf(next, sizeof(next), "%s", app->skip_path);
                snprintf(app->title, sizeof(app->title), "%s",
                         app->skip_title);
                follow = true;
            } else if (finished && !command.message[0] &&
                       app->end_action == PLAYER_END_REPEAT) {
                snprintf(next, sizeof(next), "%s", app->location);
                follow = next[0] != '\0';
            } else if (finished && !command.message[0] &&
                       app->end_action == PLAYER_END_NEXT) {
                follow = browser_ui_following_title(&app->browser,
                                                    app->location, 1, next,
                                                    sizeof(next));
                if (follow)
                    snprintf(app->title, sizeof(app->title), "%s",
                             app->browser.selected_title);
            }
            app->skip_path[0] = '\0';
            if (follow) {
                printf("player_end_action=%d next=%s\n", app->end_action,
                       next);
                app->awaiting_command = true;
                send_control(app, PLAYER_UI_FILE_SELECTED, next, 0);
                continue;
            }
            browser_ui_navigate(&app->browser, command.path, command.message);
            app->awaiting_command = false;
        }
    }
}

/* Passthrough shows the room where nothing is drawn, if the viewer chose
 * it and the runtime blends with the room (the Frame since SteamVR's
 * update of 2026-10-10; not the PC streaming to it). */
static bool passthrough_active(const struct app *app)
{
    return app->passthrough && app->xr.alpha_blend_supported;
}

/* Premultiplied colour where there is no video (clear colour, outside the
 * 180 degree image, around the flat screen, keyed out): the usual near
 * black, or with passthrough black at the dimming's opacity over the
 * room. */
static void background_color(const struct app *app, float color[4])
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

/* Weight of the luma difference against the chroma difference when
 * keying a picked colour (shading of the background stays transparent). */
#define KEY_LUMA_WEIGHT 0.4f

/* Chroma key of the title for the video shaders (chroma_key in
 * nv12-projection.frag): green and blue as directions in the Cb/Cr plane
 * (BT.709, normalized chroma), a picked colour as chroma and luma;
 * threshold, softness and spill removal. */
static void chroma_key_uniform(const struct app *app, float key[4],
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
static bool video_uv_at_ray(const struct app *app, XrVector3f origin,
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

static XrEnvironmentBlendMode environment_blend_mode(const struct app *app)
{
    return passthrough_active(app) ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                                   : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
}

/* Takes the head pose as home: its position and the direction the face
 * points in. With `panel` the player panel moves in front of it. */
static void set_home(struct app *app, XrPosef head, bool panel)
{
    XrVector3f forward = rotate_vector(head.orientation,
                                       (XrVector3f){0, 0, -1});
    XrVector3f up = rotate_vector(head.orientation, (XrVector3f){0, 1, 0});
    /* Looking down the top of the head points forward, looking up it
     * points back: the heading holds even for a headset woken up facing
     * the ceiling. */
    float x = forward.x - up.x * forward.y;
    float z = forward.z - up.z * forward.y;
    app->home_yaw = atan2f(-x, -z);
    app->home_position = head.position;
    app->home_pending = false;
    if (panel) {
        app->panel_x = head.position.x - sinf(app->home_yaw) * 1.3f;
        app->panel_y = head.position.y - 0.35f;
        app->panel_z = head.position.z - cosf(app->home_yaw) * 1.3f;
        app->panel_yaw = app->home_yaw;
    }
    printf("home x=%.3f y=%.3f z=%.3f yaw=%.1f\n", head.position.x,
           head.position.y, head.position.z,
           app->home_yaw * 180.0f / PI_F);
}

/* Sets home at the start and after SteamVR's recenter (once the changed
 * space applies), turning a playing title with it. */
static void update_home(struct app *app, XrTime time)
{
    if (!app->home_pending || time < app->home_after)
        return;
    XrSpaceLocation head = {.type = XR_TYPE_SPACE_LOCATION};
    need_xr("xrLocateSpace(home)",
            xrLocateSpace(app->xr.view_space, app->xr.space, time, &head));
    const XrSpaceLocationFlags valid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((head.locationFlags & valid) != valid)
        return;
    set_home(app, head.pose, true);
    if (app->playing) {
        app->recenter_pending = true;
        app->recenter_yaw = false;
        app->dome_yaw = -app->home_yaw;
        send_panel_state(app);
    }
}

static void note_max(double *max, double value)
{
    if (value > *max)
        *max = value;
}

/* After xrWaitFrame: counts the frame and missed display periods and logs
 * the window when it is over. */
static void timing_frame(struct app *app, const XrFrameState *state,
                         double waited_ms)
{
    struct frame_timing *timing = &app->timing;
    double now = monotonic_seconds();
    if (timing->window_start == 0.0)
        timing->window_start = now;
    XrDuration period = state->predictedDisplayPeriod;
    if (timing->last_display && period > 0) {
        XrDuration delta = state->predictedDisplayTime - timing->last_display;
        if (delta > period * 3 / 2)
            timing->missed += (uint32_t)((delta + period / 2) / period) - 1U;
    }
    timing->last_display = state->predictedDisplayTime;
    timing->period_ms = period / 1e6;
    timing->frames++;
    timing->wait_ms += waited_ms;
    if (now - timing->window_start < FRAME_TIMING_INTERVAL)
        return;
    uint32_t rendered = timing->rendered ? timing->rendered : 1U;
    uint32_t still = timing->gpu_frames[0] ? timing->gpu_frames[0] : 1U;
    uint32_t copied = timing->gpu_frames[1] ? timing->gpu_frames[1] : 1U;
    uint32_t frames = timing->frames;
    printf("frame_timing frames=%u rendered=%u missed=%u period_ms=%.1f"
           " wait_ms=%.1f cpu_ms=%.1f/%.1f gpu_ms=%.1f/%.1f"
           " copy_frames=%u copy_gpu_ms=%.1f/%.1f ui=%u ui_ms=%.1f/%.1f\n",
           frames, timing->rendered, timing->missed, timing->period_ms,
           timing->wait_ms / frames, timing->cpu_ms / rendered,
           timing->cpu_max_ms, timing->gpu_ms[0] / still,
           timing->gpu_max_ms[0], timing->gpu_frames[1],
           timing->gpu_ms[1] / copied, timing->gpu_max_ms[1],
           timing->ui_redraws,
           timing->ui_redraws ? timing->ui_ms / timing->ui_redraws : 0.0,
           timing->ui_max_ms);
    *timing = (struct frame_timing){.window_start = now,
                                    .last_display = timing->last_display};
}

void timing_ui(struct app *app, double started)
{
    double ms = (monotonic_seconds() - started) * 1e3;
    app->timing.ui_redraws++;
    app->timing.ui_ms += ms;
    note_max(&app->timing.ui_max_ms, ms);
}

static void render_frame(struct app *app)
{
    if (app->overlay.render_models_dirty) {
        app->overlay.render_models_dirty = false;
        render_models_load(&app->overlay.render_models, app->xr.instance, app->xr.session,
                           app->input.hands);
        upload_render_models(app);
    }
    XrFrameState frame_state = {.type = XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wait = {.type = XR_TYPE_FRAME_WAIT_INFO};
    double waiting = monotonic_seconds();
    need_xr("xrWaitFrame", xrWaitFrame(app->xr.session, &wait, &frame_state));
    double frame_started = monotonic_seconds();
    timing_frame(app, &frame_state, (frame_started - waiting) * 1e3);
    bool copied_video = false;
    XrFrameBeginInfo begin = {.type = XR_TYPE_FRAME_BEGIN_INFO};
    need_xr("xrBeginFrame", xrBeginFrame(app->xr.session, &begin));
    ++app->xr_frames;
    app->xr.should_render = frame_state.shouldRender;
    update_home(app, frame_state.predictedDisplayTime);
    if (app->playing) {
        sync_player_input(app, frame_state.predictedDisplayTime);
        sync_player_panel(app);
    } else {
        sync_browser_input(app, frame_state.predictedDisplayTime);
        if (app->browser.dirty) {
            double drawing = monotonic_seconds();
            upload_browser(app);
            timing_ui(app, drawing);
        }
    }
    double inputs_done = monotonic_seconds();
    if (!frame_state.shouldRender) {
        XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = frame_state.predictedDisplayTime,
            .environmentBlendMode = environment_blend_mode(app)};
        need_xr("xrEndFrame(empty)", xrEndFrame(app->xr.session, &end));
        return;
    }
    /* Without a decoded frame (browser, start, seek preroll) only the clear
     * colour and the panel are shown, so the headset never falls back to the
     * Steam environment. */
    struct video_buffer *video = NULL;
    if (app->playing && app->video.current >= 0)
        video = &app->video.buffers[app->video.current];
    else if (app->playing && app->video.has_held)
        video = &app->video.held;
    XrView views[2] = {{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
    XrViewState view_state = {.type = XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    XrViewLocateInfo locate = {.type = XR_TYPE_VIEW_LOCATE_INFO,
        .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        .displayTime = frame_state.predictedDisplayTime, .space = app->xr.space};
    need_xr("xrLocateViews", xrLocateViews(app->xr.session, &locate, &view_state, 2, &count, views));
    need(count == 2 && (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
             (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT), "OpenXR returned no valid stereo pose");
    XrSpaceLocation center = {.type = XR_TYPE_SPACE_LOCATION};
    need_xr("xrLocateSpace(VIEW)",
            xrLocateSpace(app->xr.view_space, app->xr.space,
                          frame_state.predictedDisplayTime, &center));
    need((center.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0,
         "OpenXR returned no valid VIEW orientation");
    if (!app->xr.view_geometry_logged) {
        printf("openxr_view_geometry"
               " left_pose=(%.9f,%.9f,%.9f;%.9f,%.9f,%.9f,%.9f)"
               " left_fov=(%.9f,%.9f,%.9f,%.9f)"
               " right_pose=(%.9f,%.9f,%.9f;%.9f,%.9f,%.9f,%.9f)"
               " right_fov=(%.9f,%.9f,%.9f,%.9f)"
               " center_pose=(%.9f,%.9f,%.9f;%.9f,%.9f,%.9f,%.9f)\n",
               views[0].pose.position.x, views[0].pose.position.y,
               views[0].pose.position.z, views[0].pose.orientation.x,
               views[0].pose.orientation.y, views[0].pose.orientation.z,
               views[0].pose.orientation.w, views[0].fov.angleLeft,
               views[0].fov.angleRight, views[0].fov.angleDown,
               views[0].fov.angleUp, views[1].pose.position.x,
               views[1].pose.position.y, views[1].pose.position.z,
               views[1].pose.orientation.x, views[1].pose.orientation.y,
               views[1].pose.orientation.z, views[1].pose.orientation.w,
               views[1].fov.angleLeft, views[1].fov.angleRight,
               views[1].fov.angleDown, views[1].fov.angleUp,
               center.pose.position.x, center.pose.position.y,
               center.pose.position.z, center.pose.orientation.x,
               center.pose.orientation.y, center.pose.orientation.z,
               center.pose.orientation.w);
        app->xr.view_geometry_logged = true;
    }
    XrSwapchainImageAcquireInfo acquire = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t image_index = 0;
    need_xr("xrAcquireSwapchainImage", xrAcquireSwapchainImage(app->xr.swapchain, &acquire, &image_index));
    XrSwapchainImageWaitInfo image_wait = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
    need_xr("xrWaitSwapchainImage", xrWaitSwapchainImage(app->xr.swapchain, &image_wait));
    need(image_index < app->xr.image_count, "invalid OpenXR swapchain index");
    double acquired = monotonic_seconds();
    /* Dome placement: centred on the head of the first frame (or of
     * "zentrieren"), turned by the gaze yaw and the viewer's rotation.
     * world_to_dome undoes yaw, then tilt, then roll. */
    if (app->recenter_pending &&
        (center.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        app->dome_center = center.pose.position;
        if (app->recenter_yaw) {
            set_home(app, center.pose, false);
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
    struct mat4 world_to_dome = multiply(
        rotation_axis(2, roll),
        multiply(rotation_axis(0, -tilt), rotation_axis(1, yaw)));
    struct mat4 dome_to_world = multiply(
        rotation_axis(1, -yaw),
        multiply(rotation_axis(0, tilt), rotation_axis(2, -roll)));
    /* Eye inside the dome (radius 1 m): head motion, then zoom, height and
     * slide along the dome axes. */
    float motion = app->adjust[PLAYER_ADJUST_HEAD_MOTION];
    XrVector3f head = {
        (center.pose.position.x - app->dome_center.x) * motion,
        (center.pose.position.y - app->dome_center.y) * motion,
        (center.pose.position.z - app->dome_center.z) * motion,
    };
    XrVector3f eye_position = transform_direction(world_to_dome, head);
    eye_position.x -= app->adjust[PLAYER_ADJUST_SLIDE];
    eye_position.y -= app->adjust[PLAYER_ADJUST_HEIGHT];
    eye_position.z -= app->adjust[PLAYER_ADJUST_ZOOM];
    float eye_distance = sqrtf(vector_dot(eye_position, eye_position));
    if (eye_distance > 0.9f) {
        eye_position.x *= 0.9f / eye_distance;
        eye_position.y *= 0.9f / eye_distance;
        eye_position.z *= 0.9f / eye_distance;
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
        dome_to_world,
        (XrVector3f){app->adjust[PLAYER_ADJUST_SLIDE],
                     app->adjust[PLAYER_ADJUST_HEIGHT],
                     -(2.0f - app->adjust[PLAYER_ADJUST_ZOOM])});
    struct mat4 screen_model = dome_to_world;
    screen_model.v[12] = app->dome_center.x + screen.x;
    screen_model.v[13] = app->dome_center.y + screen.y;
    screen_model.v[14] = app->dome_center.z + screen.z;
    struct mat4 screen_scale = {{2.0f, 0, 0, 0, 0, screen_height, 0, 0,
                                 0, 0, 1, 0, 0, 0, 0, 1}};
    screen_model = multiply(screen_model, screen_scale);
    /* Autofocus: measure every few frames where the viewer looks. */
    advance_autofocus(app);
    bool gaze = false;
    XrVector3f look = view_direction(app, center.pose,
                                     frame_state.predictedDisplayTime, &gaze);
    app->autofocus.gaze = gaze;
    app->autofocus.gaze_marker_visible = app->gaze_debug && video;
    if (app->autofocus.gaze_marker_visible) {
        app->autofocus.gaze_marker_pose = (XrPosef){
            center.pose.orientation,
            {center.pose.position.x + look.x * GAZE_MARKER_DISTANCE,
             center.pose.position.y + look.y * GAZE_MARKER_DISTANCE,
             center.pose.position.z + look.z * GAZE_MARKER_DISTANCE}};
    }
    update_debug_text(app);
    struct disparity_push disparity_push;
    bool measure =
        video &&
        (app->adjust[PLAYER_ADJUST_AUTOFOCUS] > 0.0f || app->gaze_debug) &&
        ++app->autofocus.disparity_frames % AUTOFOCUS_EVERY_FRAMES == 0U &&
        disparity_window(app, look, world_to_dome, eye_position,
                         app->adjust[PLAYER_ADJUST_STEREO_X] +
                             app->autofocus.offset,
                         &disparity_push);
    /* A key colour pick takes the measurement's turn. */
    bool key_sample = false;
    if (app->autofocus.key_pick_request && video) {
        app->autofocus.key_pick_request = false;
        float uv[2];
        XrVector3f aim = rotate_vector(app->autofocus.key_pick_aim.orientation,
                                       (XrVector3f){0, 0, -1});
        if (video_uv_at_ray(app, app->autofocus.key_pick_aim.position, aim,
                            center.pose.position, world_to_dome,
                            eye_position, screen_model, uv, NULL)) {
            float step_u = app->video.width ? 1.5f / (float)app->video.width
                                            : 0.0f;
            float step_v = app->video.height
                               ? 1.5f / (float)app->video.height
                               : 0.0f;
            disparity_push = (struct disparity_push){
                .left = {uv[0], uv[1], step_u, step_v},
                .bounds = {0.0f, 1.0f,
                           app->video.coded_width ? (float)app->video.width /
                                                  (float)app->video.coded_width
                                            : 1.0f,
                           app->video.coded_height ? (float)app->video.height /
                                                   (float)app->video.coded_height
                                             : 1.0f},
                .sample_mode = {1.0f, 0.0f, 0.0f, 0.0f},
            };
            key_sample = measure = true;
            printf("key_pick u=%.4f v=%.4f\n", uv[0], uv[1]);
        } else {
            printf("key_pick missed the picture\n");
        }
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        struct mat4 projection = projection_from_fov(views[eye].fov);
        struct mat4 view = view_from_pose(views[eye].pose);
        float side = eye == 0 ? -1.0f : 1.0f;
        float stereo_x = (app->adjust[PLAYER_ADJUST_STEREO_X] +
                          app->autofocus.offset) * 0.5f;
        float stereo_y = app->adjust[PLAYER_ADJUST_STEREO_Y] * 0.5f;
        struct view_uniform uniform = {
            .mvp = multiply(projection, multiply(view, screen_model)),
            .camera_to_world = multiply(world_to_dome,
                                        rotation_from_pose(views[eye].pose)),
            /* w: half the fisheye lens angle (radians). */
            .eye_offset = {eye_position.x, eye_position.y, eye_position.z,
                           0.5f * (float)app->fisheye_fov * degrees},
            .stereo_offset = {-side * stereo_x * degrees,
                              -side * stereo_y * degrees,
                              -side * stereo_x / 60.0f,
                              side * stereo_y / 60.0f},
            .picture = {app->adjust[PLAYER_ADJUST_BRIGHTNESS],
                        app->adjust[PLAYER_ADJUST_CONTRAST],
                        app->adjust[PLAYER_ADJUST_SATURATION],
                        app->adjust[PLAYER_ADJUST_GAMMA]},
            .fov_tangents = {
                tanf(views[eye].fov.angleLeft),
                tanf(views[eye].fov.angleRight),
                tanf(views[eye].fov.angleDown),
                tanf(views[eye].fov.angleUp),
            },
            .modes = {
                (int32_t)app->projection_mode,
                (int32_t)app->stereo_layout,
                (int32_t)eye,
                app->swap_eyes,
            },
            .source_info = {
                (float)app->video.width,
                (float)app->video.height,
                app->video.width ? 1.0f / (float)app->video.width : 0.0f,
                app->video.height ? 1.0f / (float)app->video.height : 0.0f,
            },
            .video_scale = {
                app->video.coded_width ? (float)app->video.width /
                                       (float)app->video.coded_width : 1.0f,
                app->video.coded_height ? (float)app->video.height /
                                        (float)app->video.coded_height : 1.0f,
                0.0f, 0.0f,
            },
        };
        video_levels_uniform(app->video_levels, uniform.levels);
        background_color(app, uniform.background);
        chroma_key_uniform(app, uniform.chroma_key, uniform.chroma_spill);
        memcpy((uint8_t *)app->uniform_mapping + eye * UBO_STRIDE,
               &uniform, sizeof(uniform));
    }
    need_vk("vkWaitForFences(render)", vkWaitForFences(app->gpu.device, 1, &app->fence, VK_TRUE, UINT64_C(5000000000)));
    need_vk("vkResetFences(render)", vkResetFences(app->gpu.device, 1, &app->fence));
    double fenced = monotonic_seconds();
    if (app->autofocus.key_sample_pending) {
        app->autofocus.key_sample_pending = false;
        finish_key_pick(app);
    } else if (app->autofocus.disparity_pending) {
        app->autofocus.disparity_pending = false;
        update_autofocus(app);
    }
    need_vk("vkResetCommandPool(render)", vkResetCommandPool(app->gpu.device, app->command_pool, 0));
    VkCommandBufferBeginInfo command_begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    need_vk("vkBeginCommandBuffer(render)", vkBeginCommandBuffer(app->command, &command_begin));
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image = app->xr.images[image_index].image, .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 2}};
    vkCmdPipelineBarrier(app->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkImageMemoryBarrier video_acquire = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = app->gpu.queue_family,
        .image = video ? video->image : VK_NULL_HANDLE,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    if (video && video->host) {
        /* Own images, not shared with a foreign producer. */
        video_acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        video_acquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        if (video->producer_wait_pending) {
            record_host_upload(app, video);
            video->producer_wait_pending = false;
            copied_video = true;
        }
    }
    VkImageMemoryBarrier video_acquires[2] = {video_acquire, video_acquire};
    uint32_t video_images = video && video->tp10 ? 2U : 1U;
    if (video_images == 2U)
        video_acquires[1].image = video->image_uv;
    if (video)
        vkCmdPipelineBarrier(app->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, NULL, 0, NULL, video_images,
                             video_acquires);
    if (measure) {
        vkCmdBindPipeline(app->command, VK_PIPELINE_BIND_POINT_COMPUTE,
                          video->tp10 ? app->autofocus.disparity_pipeline_tp10
                                      : app->autofocus.disparity_pipeline);
        uint32_t offset = 0;
        vkCmdBindDescriptorSets(app->command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                app->pipeline_layout, 0, 1,
                                &video->descriptor_set, 1, &offset);
        vkCmdPushConstants(app->command, app->pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(disparity_push), &disparity_push);
        vkCmdDispatch(app->command, 1, 1, 1);
        app->autofocus.key_sample_pending = key_sample;
        VkMemoryBarrier to_host = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        vkCmdPipelineBarrier(app->command,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0,
                             NULL, 0, NULL);
        app->autofocus.disparity_pending = !key_sample;
    }
    XrCompositionLayerQuad ui_layer = app->playing
                                          ? make_ui_layer(app)
                                          : make_browser_layer(app);
    bool show_ui = app->playing ? app->ui_visible : true;
    const struct ui_texture *panel = NULL;
    if (app->panels.panels_in_eye_images && show_ui)
        panel = app->playing ? &app->panels.ui_texture : &app->panels.browser_texture;
    struct mat4 panel_matrix = panel_model(&ui_layer);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        VkClearValue clear;
        background_color(app, clear.color.float32);
        VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = app->render_pass, .framebuffer = app->framebuffers[image_index][eye],
            .renderArea = {.extent = {app->xr.width, app->xr.height}}, .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(app->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        if (video) {
            vkCmdBindPipeline(app->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              video->tp10 ? app->pipeline_tp10 : app->pipeline);
            uint32_t offset = eye * UBO_STRIDE;
            vkCmdBindDescriptorSets(app->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    app->pipeline_layout, 0, 1,
                                    &video->descriptor_set, 1, &offset);
            vkCmdDraw(app->command, 4, 1, 0, 0);
        }
        vkCmdEndRenderPass(app->command);
    }
    VkImageMemoryBarrier video_release = video_acquire;
    video_release.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    video_release.dstAccessMask = 0;
    if (!video || !video->host) {
        video_release.srcQueueFamilyIndex = app->gpu.queue_family;
        video_release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    }
    VkImageMemoryBarrier video_releases[2] = {video_release, video_release};
    if (video_images == 2U)
        video_releases[1].image = video->image_uv;
    if (video)
        vkCmdPipelineBarrier(app->command,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                             0, NULL, video_images, video_releases);
    /* Beams end at the panel they hit, else at the video (with a dot
     * while a key colour is picked), else after a fixed length. */
    bool overlay = app->overlay.wanted;
    uint32_t overlay_indices[2] = {0, 0};
    if (overlay) {
        float beam_lengths[2];
        bool beam_dots[2] = {false, false};
        for (unsigned hand = 0; hand < 2; ++hand) {
            const struct laser *laser = &app->overlay.lasers[hand];
            beam_lengths[hand] = BEAM_DEFAULT_LENGTH;
            float uv[2], distance;
            XrVector3f direction = rotate_vector(laser->aim.orientation,
                                                 (XrVector3f){0, 0, -1});
            if (laser->panel_length > 0.0f) {
                beam_lengths[hand] = laser->panel_length;
                beam_dots[hand] = app->panels.panels_in_eye_images;
            } else if (laser->valid && video &&
                       video_uv_at_ray(app, laser->aim.position, direction,
                                       center.pose.position, world_to_dome,
                                       eye_position, screen_model, uv,
                                       &distance)) {
                beam_lengths[hand] = distance;
                beam_dots[hand] = app->key_picking;
            }
        }
        for (uint32_t eye = 0; eye < 2 && app->overlay.in_eye_images; ++eye)
            overlay_indices[eye] = image_index;
        for (uint32_t eye = 0; eye < 2 && !app->overlay.in_eye_images;
             ++eye) {
            XrSwapchainImageAcquireInfo overlay_acquire = {
                .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
            };
            need_xr("xrAcquireSwapchainImage(overlay)",
                    xrAcquireSwapchainImage(app->overlay.swapchains[eye],
                                            &overlay_acquire,
                                            &overlay_indices[eye]));
            XrSwapchainImageWaitInfo overlay_wait = {
                .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
                .timeout = XR_INFINITE_DURATION,
            };
            need_xr("xrWaitSwapchainImage(overlay)",
                    xrWaitSwapchainImage(app->overlay.swapchains[eye],
                                         &overlay_wait));
            need(overlay_indices[eye] < app->overlay.image_counts[eye],
                 "invalid overlay swapchain index");
        }
        record_overlay(app, app->command, overlay_indices, views,
                       frame_state.predictedDisplayTime, panel, panel_matrix,
                       beam_lengths, beam_dots);
    }
    need_vk("vkEndCommandBuffer(render)", vkEndCommandBuffer(app->command));
    VkPipelineStageFlags producer_stage =
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &app->command};
    if (video && video->producer_wait_pending) {
        struct dma_buf_export_sync_file sync_export = {
            .flags = DMA_BUF_SYNC_READ, .fd = -1,
        };
        if (ioctl(video->dma_buf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE,
                  &sync_export) < 0) {
            perror("DMA_BUF_IOCTL_EXPORT_SYNC_FILE");
            exit(2);
        }
        VkImportSemaphoreFdInfoKHR semaphore_import = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = video->producer_semaphore,
            .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = sync_export.fd,
        };
        need_vk("vkImportSemaphoreFdKHR(producer)",
                app->gpu.import_semaphore_fd(app->gpu.device, &semaphore_import));
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &video->producer_semaphore;
        submit.pWaitDstStageMask = &producer_stage;
        video->producer_wait_pending = false;
    }
    double recorded = monotonic_seconds();
    need_vk("vkQueueSubmit(render)", vkQueueSubmit(app->gpu.queue, 1, &submit, app->fence));
    double submitted = monotonic_seconds();
    need_vk("vkWaitForFences(render completion)", vkWaitForFences(app->gpu.device, 1, &app->fence, VK_TRUE, UINT64_C(5000000000)));
    double gpu_ms = (monotonic_seconds() - submitted) * 1e3;
    app->timing.gpu_ms[copied_video] += gpu_ms;
    app->timing.gpu_frames[copied_video]++;
    note_max(&app->timing.gpu_max_ms[copied_video], gpu_ms);
    XrSwapchainImageReleaseInfo release = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    need_xr("xrReleaseSwapchainImage", xrReleaseSwapchainImage(app->xr.swapchain, &release));
    for (uint32_t eye = 0;
         overlay && !app->overlay.in_eye_images && eye < 2; ++eye)
        need_xr("xrReleaseSwapchainImage(overlay)",
                xrReleaseSwapchainImage(app->overlay.swapchains[eye],
                                        &release));
    XrCompositionLayerProjectionView overlay_views[2];
    for (uint32_t eye = 0; eye < 2; ++eye)
        overlay_views[eye] = (XrCompositionLayerProjectionView){
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
            .pose = views[eye].pose, .fov = views[eye].fov,
            .subImage = {.swapchain = app->overlay.swapchains[eye],
                         .imageRect = {.extent = {
                             (int32_t)app->overlay.width,
                             (int32_t)app->overlay.height}}},
        };
    XrCompositionLayerProjection overlay_layer = {
        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->xr.space, .viewCount = 2, .views = overlay_views,
    };
    XrCompositionLayerProjectionView projection_views[2] = {{.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (uint32_t eye = 0; eye < 2; ++eye) {
        projection_views[eye].pose = views[eye].pose; projection_views[eye].fov = views[eye].fov;
        projection_views[eye].subImage = (XrSwapchainSubImage){.swapchain = app->xr.swapchain, .imageRect = {.extent = {(int32_t)app->xr.width, (int32_t)app->xr.height}}, .imageArrayIndex = eye};
    }
    XrCompositionLayerProjection layer = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        .layerFlags = passthrough_active(app)
                          ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                          : 0,
        .space = app->xr.space, .viewCount = 2, .views = projection_views};
    XrCompositionLayerQuad pointer_layer = make_pointer_layer(app);
    XrCompositionLayerQuad gaze_layer = make_gaze_marker_layer(app);
    const XrCompositionLayerBaseHeader *layers[5] = {
        (const XrCompositionLayerBaseHeader *)&layer,
    };
    uint32_t layer_count = 1;
    if (app->playing && app->autofocus.gaze_marker_visible)
        layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&gaze_layer;
    if (show_ui) {
        if (!app->panels.panels_in_eye_images)
            layers[layer_count++] =
                (const XrCompositionLayerBaseHeader *)&ui_layer;
    }
    if (overlay && !app->overlay.in_eye_images)
        layers[layer_count++] =
            (const XrCompositionLayerBaseHeader *)&overlay_layer;
    if (show_ui && app->panels.pointer_layer_visible && !app->panels.panels_in_eye_images)
        layers[layer_count++] =
            (const XrCompositionLayerBaseHeader *)&pointer_layer;
    XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = frame_state.predictedDisplayTime,
        .environmentBlendMode = environment_blend_mode(app),
        .layerCount = layer_count,
        .layers = layers};
    double ending = monotonic_seconds();
    need_xr("xrEndFrame", xrEndFrame(app->xr.session, &end));
    ++app->rendered_frames;
    double cpu_ms = (monotonic_seconds() - frame_started) * 1e3;
    if (cpu_ms > 50.0)
        printf("slow_frame ms=%.0f inputs=%.0f acquire=%.0f fence=%.0f"
               " record=%.0f submit=%.0f gpu=%.0f end=%.0f copy=%d\n",
               cpu_ms, (inputs_done - frame_started) * 1e3,
               (acquired - inputs_done) * 1e3, (fenced - acquired) * 1e3,
               (recorded - fenced) * 1e3, (submitted - recorded) * 1e3,
               gpu_ms, (monotonic_seconds() - ending) * 1e3, copied_video);
    app->timing.rendered++;
    app->timing.cpu_ms += cpu_ms;
    note_max(&app->timing.cpu_max_ms, cpu_ms);
    if (!video)
        return;
    if (app->video.presented)
        ++app->repeated_video_frames;
    else {
        ++app->unique_video_frames;
        app->video.presented = true;
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2) {
        fprintf(stderr, "usage: %s UI_FD\n", argv[0]);
        return 2;
    }
    struct app app = {0};
    app.ui_fd = (int)parse_u64(argv[1], "UI fd");
    app.link.fd = -1;
    app.video.buffer_count = VIDEO_BUFFER_COUNT;
    for (uint32_t i = 0; i < NV12_IPC_MAX_BUFFERS; ++i)
        app.video.buffers[i].dma_buf_fd = -1;
    app.video.held.dma_buf_fd = -1;
    app.video.current = -1;
    app.link.player_state = NV12_PLAYER_PLAYING;
    app.ui_visible = true;
    app.ui_dirty = true;
    app.panel_y = -0.35f;
    app.panel_z = -1.3f;
    app.grab_hand = -1;
    app.scrub_hand = -1;
    app.bar_u = -1.0f;
    app.controls.learn_action = -1;
    app.home_pending = true;
    app.settings_tab = PLAYER_UI_TAB_PLAYBACK;
    app.awaiting_frame = true;
    char language[16];
    settings_get_string("language", language, sizeof(language), "auto");
    app.system_language = i18n_detect();
    printf("language=%s choice=%s system=%s\n",
           i18n_languages[i18n_select(language)]->code, language,
           i18n_languages[app.system_language]->code);
    updater_init(&app.updater);
    app.update_auto = settings_get_bool("update_check", true);
    char browser_error[256] = {0};
    if (browser_ui_open(&app.browser, "root:", NULL, browser_error,
                        sizeof(browser_error)) < 0) {
        fprintf(stderr, "browser_error=%s\n", browser_error);
        return 2;
    }
    app.trickplay = trickplay_create();
    load_preview_settings(&app);
    load_adjust(&app, true);
    if (app.update_auto)
        updater_check(&app.updater);
    refresh_update_status(&app);
    initialize_xr_vulkan(&app);
    XrGraphicsBindingVulkan2KHR binding = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = app.gpu.instance, .physicalDevice = app.gpu.physical, .device = app.gpu.device, .queueFamilyIndex = app.gpu.queue_family, .queueIndex = 0};
    XrSessionCreateInfo session_info = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = app.xr.system};
    need_xr("xrCreateSession", xrCreateSession(app.xr.instance, &session_info, &app.xr.session));
    initialize_actions(&app);
    /* SteamVR on Frame requires READY/xrBeginSession before it permits a
     * reference space or swapchain to be created. */
    struct timespec pause = {.tv_nsec = 10000000};
    struct timespec started, now;
    clock_gettime(CLOCK_MONOTONIC, &started);
    while (!app.xr.running) {
        poll_events(&app);
        clock_gettime(CLOCK_MONOTONIC, &now);
        need(now.tv_sec - started.tv_sec < 10,
             "OpenXR session did not enter READY within ten seconds");
        if (!app.xr.running) nanosleep(&pause, NULL);
    }
    XrReferenceSpaceCreateInfo space_info = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL, .poseInReferenceSpace = {{0,0,0,1},{0,0,0}}};
    need_xr("xrCreateReferenceSpace", xrCreateReferenceSpace(app.xr.session, &space_info, &app.xr.space));
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    need_xr("xrCreateReferenceSpace(VIEW)",
            xrCreateReferenceSpace(app.xr.session, &space_info, &app.xr.view_space));
    initialize_swapchain(&app);
    app.panels.panels_in_eye_images = panels_in_eye_images();
    create_ui_swapchain(&app);
    create_video_sampler(&app);
    create_render_resources(&app);
    create_ui_resources(&app);
    start_panel_thread(&app);
    create_browser_texture(&app);
    create_overlay(&app);
    create_panel_pipeline(&app);
    create_pointer_texture(&app);
    create_gaze_markers(&app);
    printf("host_ready\n");
    while (!app.quit) {
        handle_control(&app);
        if (app.playing) {
            receive_frame_messages(&app);
            struct ui_image *strip[TRICKPLAY_STRIP];
            if (trickplay_poll(app.trickplay, strip)) {
                for (int i = 0; i < TRICKPLAY_STRIP; ++i) {
                    ui_image_free(app.preview[i]);
                    app.preview[i] = strip[i];
                }
                if (app.preview_visible)
                    app.preview_redraw_pending = true; /* update_preview */
            }
            if (trickplay_details(app.trickplay, &app.details)) {
                app.details_valid = true;
                app.ui_dirty = true;
            }
        }
        browser_ui_update(&app.browser);
        poll_updater(&app);
        poll_events(&app);
        hide_dashboard_at_startup(&app);
        if (app.quit)
            break;
        if (app.xr.running) render_frame(&app); else nanosleep(&pause, NULL);
        if (app.playing && app.link.eos_received)
            end_playback(&app);
    }
    if (app.xr_exit)
        send_control(&app, PLAYER_UI_EXIT_REQUESTED, NULL, 0);
    if (app.playing)
        end_playback(&app);
    updater_close(&app.updater);
    browser_ui_close(&app.browser);
    stop_panel_thread(&app);
    player_ui_close(&app.panels.player_ui);
    trickplay_destroy(app.trickplay);
    for (int i = 0; i < TRICKPLAY_STRIP; ++i)
        ui_image_free(app.preview[i]);
    return 0;
}
