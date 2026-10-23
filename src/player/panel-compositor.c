#define _POSIX_C_SOURCE 200809L

#include "renderer.h"

#define BROWSER_PANEL_DISTANCE 2.4f
#define POINTER_PIXELS 64U

/* Draws the settings page over the browser picture, which it dims. */
static void compose_settings(struct app *app)
{
    if (!app->panels.settings_cpu) {
        app->panels.settings_cpu = malloc((size_t)PLAYER_UI_WIDTH *
                                   PLAYER_UI_HEIGHT * 4U);
        need(app->panels.settings_cpu != NULL, "allocate settings pixels");
    }
    struct player_ui_state state = settings_state(app);
    pthread_mutex_lock(&app->panels.player_ui_mutex);
    player_ui_render_settings(&app->panels.player_ui, &state,
                              app->panels.settings_cpu, app->panels.ui_bgra);
    pthread_mutex_unlock(&app->panels.player_ui_mutex);
    uint8_t *browser = (uint8_t *)app->panels.browser_cpu;
    for (size_t i = 0; i < (size_t)BROWSER_UI_WIDTH * BROWSER_UI_HEIGHT;
         ++i)
        for (int c = 0; c < 4; ++c)
            browser[i * 4 + (size_t)c] =
                (uint8_t)(browser[i * 4 + (size_t)c] * 2U / 5U +
                          (c == 3 ? 153U : 0U));
    const uint8_t *page = (const uint8_t *)app->panels.settings_cpu;
    for (int y = 0; y < PLAYER_UI_POPUP_H; ++y)
        for (int x = 0; x < PLAYER_UI_POPUP_W; ++x) {
            const uint8_t *source =
                page + ((size_t)(PLAYER_UI_POPUP_Y + y) * PLAYER_UI_WIDTH +
                        (size_t)(PLAYER_UI_POPUP_X + x)) * 4U;
            uint8_t *target =
                browser + ((size_t)(SETTINGS_Y + y) * BROWSER_UI_WIDTH +
                           (size_t)(SETTINGS_X + x)) * 4U;
            unsigned keep = 255U - source[3];
            for (int c = 0; c < 4; ++c)
                target[c] = (uint8_t)(source[c] +
                                      (target[c] * keep + 127U) / 255U);
        }
}

/* Where the panels are drawn. SteamVR makes quad layers overlays of its
 * own, which it composites over every projection layer: the controllers
 * and pointer beams (overlay layer, see record_overlay) vanished behind a
 * panel. On a PC quad layers also shimmer and step while the head moves.
 * The panels are therefore drawn into the overlay layer's eye images, from
 * mipmapped images with trilinear and anisotropic filtering, below the
 * controllers; the overlay has a higher resolution than the video for the
 * text (OVERLAY_SCALE). STEREONAUT_UI_LAYER=quad brings the quad layers
 * back (controllers then disappear behind the panels). */
bool panels_in_eye_images(void)
{
    const char *forced = getenv("STEREONAUT_UI_LAYER");
    return !(forced && strcmp(forced, "quad") == 0);
}

/* A panel's quad-layer swapchain or, drawn into the eye images, its
 * mipmapped image (then no swapchain). */
static XrSwapchain create_ui_texture(struct app *app,
                                     struct ui_texture *texture,
                                     uint32_t width, uint32_t height,
                                     const char *what)
{
    *texture = (struct ui_texture){.width = width, .height = height};
    if (!app->panels.panels_in_eye_images) {
        XrSwapchainCreateInfo info = {
            .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                          XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
            .format = app->panels.ui_format,
            .sampleCount = 1,
            .width = width,
            .height = height,
            .faceCount = 1,
            .arraySize = 1,
            .mipCount = 1,
        };
        XrSwapchain swapchain = XR_NULL_HANDLE;
        need_xr(what, xrCreateSwapchain(app->xr.session, &info, &swapchain));
        return swapchain;
    }
    uint32_t size = width > height ? width : height;
    while (size >> texture->mip_levels)
        ++texture->mip_levels;
    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = app->panels.ui_format,
        .extent = {width, height, 1}, .mipLevels = texture->mip_levels,
        .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    need_vk("vkCreateImage(panel)",
            vkCreateImage(app->gpu.device, &image, NULL, &texture->image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(app->gpu.device, texture->image,
                                 &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(app->gpu.physical,
                                       requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    need_vk("vkAllocateMemory(panel)",
            vkAllocateMemory(app->gpu.device, &allocation, NULL,
                             &texture->memory));
    need_vk("vkBindImageMemory(panel)",
            vkBindImageMemory(app->gpu.device, texture->image, texture->memory,
                              0));
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = app->panels.ui_format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = texture->mip_levels,
                             .layerCount = 1},
    };
    need_vk("vkCreateImageView(panel)",
            vkCreateImageView(app->gpu.device, &view, NULL, &texture->view));
    return XR_NULL_HANDLE;
}

static VkImageMemoryBarrier ui_barrier(VkImage image, uint32_t level,
                                       uint32_t levels, VkAccessFlags from,
                                       VkAccessFlags to,
                                       VkImageLayout old_layout,
                                       VkImageLayout new_layout)
{
    return (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = from, .dstAccessMask = to,
        .oldLayout = old_layout, .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel = level, .levelCount = levels,
                             .layerCount = 1},
    };
}

/* Copies the staged drawing into the quad layer's swapchain image (target)
 * or into the panel image, whose smaller mip levels are then filtered down
 * from it level by level. */
void record_ui_upload(VkCommandBuffer command,
                      struct ui_texture *texture, VkImage target,
                      VkBuffer staging, bool initialized)
{
    VkImage image = texture->image ? texture->image : target;
    uint32_t levels = texture->image ? texture->mip_levels : 1U;
    bool ready = texture->image ? texture->ready : initialized;
    VkImageMemoryBarrier to_copy = ui_barrier(
        image, 0, levels, ready ? VK_ACCESS_SHADER_READ_BIT : 0,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
              : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &to_copy);
    VkBufferImageCopy copy = {
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .layerCount = 1},
        .imageExtent = {texture->width, texture->height, 1},
    };
    vkCmdCopyBufferToImage(command, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    int32_t width = (int32_t)texture->width;
    int32_t height = (int32_t)texture->height;
    for (uint32_t level = 1; level < levels; ++level) {
        VkImageMemoryBarrier to_source = ui_barrier(
            image, level - 1, 1, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &to_source);
        int32_t next_width = width > 1 ? width / 2 : 1;
        int32_t next_height = height > 1 ? height / 2 : 1;
        VkImageBlit blit = {
            .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .mipLevel = level - 1, .layerCount = 1},
            .srcOffsets = {{0, 0, 0}, {width, height, 1}},
            .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .mipLevel = level, .layerCount = 1},
            .dstOffsets = {{0, 0, 0}, {next_width, next_height, 1}},
        };
        vkCmdBlitImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        width = next_width;
        height = next_height;
    }
    /* All but the last level are now transfer sources. */
    VkImageMemoryBarrier to_sample[2] = {
        ui_barrier(image, 0, levels - 1, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        ui_barrier(image, levels - 1, 1, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, levels > 1 ? 2U : 1U,
                         levels > 1 ? to_sample : &to_sample[1]);
    texture->ready = true;
}

void create_ui_swapchain(struct app *app)
{
    uint32_t count = 0;
    need_xr("xrEnumerateSwapchainFormats(UI count)",
            xrEnumerateSwapchainFormats(app->xr.session, 0, &count, NULL));
    int64_t *formats = calloc(count, sizeof(*formats));
    need(formats != NULL, "allocate UI swapchain formats");
    need_xr(
        "xrEnumerateSwapchainFormats(UI list)",
        xrEnumerateSwapchainFormats(app->xr.session, count, &count, formats));
    VkFormat preferred[] = {
        VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
    };
    for (size_t candidate = 0; candidate < ARRAY_COUNT(preferred); ++candidate)
        for (uint32_t i = 0; i < count; ++i)
            if (!app->panels.ui_format && formats[i] == preferred[candidate])
                app->panels.ui_format = preferred[candidate];
    free(formats);
    need(app->panels.ui_format != VK_FORMAT_UNDEFINED,
         "no supported UI swapchain format");
    app->panels.ui_bgra = app->panels.ui_format == VK_FORMAT_B8G8R8A8_SRGB ||
                   app->panels.ui_format == VK_FORMAT_B8G8R8A8_UNORM;
    app->panels.ui_swapchain =
        create_ui_texture(app, &app->panels.ui_texture, UI_WIDTH, UI_HEIGHT,
                          "xrCreateSwapchain(UI)");
    printf("ui_panel format=%d dimensions=%ux%u drawn_into=%s mips=%u\n",
           app->panels.ui_format, UI_WIDTH, UI_HEIGHT,
           app->panels.panels_in_eye_images ? "eye_images" : "quad_layer",
           app->panels.ui_texture.mip_levels);
    if (!app->panels.ui_swapchain)
        return;
    need_xr("xrEnumerateSwapchainImages(UI count)",
            xrEnumerateSwapchainImages(app->panels.ui_swapchain, 0,
                                       &app->panels.ui_image_count, NULL));
    need(app->panels.ui_image_count > 0 &&
             app->panels.ui_image_count <= MAX_IMAGES,
         "unexpected UI swapchain image count");
    for (uint32_t i = 0; i < app->panels.ui_image_count; ++i)
        app->panels.ui_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    need_xr("xrEnumerateSwapchainImages(UI list)",
            xrEnumerateSwapchainImages(
                app->panels.ui_swapchain, app->panels.ui_image_count,
                &app->panels.ui_image_count,
                (XrSwapchainImageBaseHeader *)app->panels.ui_images));
}

void create_ui_resources(struct app *app)
{
    VkCommandBufferAllocateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->render.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    need_vk("vkAllocateCommandBuffers(UI)",
            vkAllocateCommandBuffers(app->gpu.device, &commands,
                                     &app->panels.ui_command));
    VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    need_vk("vkCreateFence(UI)", vkCreateFence(app->gpu.device, &fence, NULL,
                                               &app->panels.ui_fence));
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = UI_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    need_vk("vkCreateBuffer(UI staging)",
            vkCreateBuffer(app->gpu.device, &buffer, NULL,
                           &app->panels.ui_staging_buffer));
    VkMemoryRequirements memory;
    vkGetBufferMemoryRequirements(app->gpu.device,
                                  app->panels.ui_staging_buffer, &memory);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory.size,
        .memoryTypeIndex = memory_type(
            app->gpu.physical, memory.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    need_vk("vkAllocateMemory(UI staging)",
            vkAllocateMemory(app->gpu.device, &allocation, NULL,
                             &app->panels.ui_staging_memory));
    need_vk("vkBindBufferMemory(UI staging)",
            vkBindBufferMemory(app->gpu.device, app->panels.ui_staging_buffer,
                               app->panels.ui_staging_memory, 0));
    need_vk("vkMapMemory(UI staging)",
            vkMapMemory(app->gpu.device, app->panels.ui_staging_memory, 0,
                        UI_BYTES, 0, (void **)&app->panels.ui_pixels));
    app->ui_dirty = true;
}

/* Uploads a drawn player panel into its texture or quad swapchain. */
static void upload_ui(struct app *app, const uint32_t *pixels)
{
    XrSwapchainImageAcquireInfo acquire = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
    };
    uint32_t index = 0;
    XrSwapchainImageWaitInfo wait = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = XR_INFINITE_DURATION,
    };
    if (app->panels.ui_swapchain) {
        need_xr("xrAcquireSwapchainImage(UI)",
                xrAcquireSwapchainImage(app->panels.ui_swapchain, &acquire,
                                        &index));
        need_xr("xrWaitSwapchainImage(UI)",
                xrWaitSwapchainImage(app->panels.ui_swapchain, &wait));
        need(index < app->panels.ui_image_count, "invalid UI swapchain index");
    }
    need_vk("vkWaitForFences(UI)",
            vkWaitForFences(app->gpu.device, 1, &app->panels.ui_fence,
                            VK_TRUE, UINT64_C(5000000000)));
    need_vk("vkResetFences(UI)",
            vkResetFences(app->gpu.device, 1, &app->panels.ui_fence));
    memcpy(app->panels.ui_pixels, pixels, UI_BYTES);
    need_vk("vkResetCommandBuffer(UI)",
            vkResetCommandBuffer(app->panels.ui_command, 0));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    need_vk("vkBeginCommandBuffer(UI)",
            vkBeginCommandBuffer(app->panels.ui_command, &begin));
    record_ui_upload(app->panels.ui_command, &app->panels.ui_texture,
                     app->panels.ui_images[index].image,
                     app->panels.ui_staging_buffer,
                     app->panels.ui_image_initialized[index]);
    need_vk("vkEndCommandBuffer(UI)",
            vkEndCommandBuffer(app->panels.ui_command));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &app->panels.ui_command,
    };
    need_vk("vkQueueSubmit(UI)",
            vkQueueSubmit(app->gpu.queue, 1, &submit, app->panels.ui_fence));
    need_vk("vkWaitForFences(UI completion)",
            vkWaitForFences(app->gpu.device, 1, &app->panels.ui_fence,
                            VK_TRUE, UINT64_C(5000000000)));
    app->panels.ui_image_initialized[index] = true;
    XrSwapchainImageReleaseInfo release = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
    };
    if (app->panels.ui_swapchain)
        need_xr("xrReleaseSwapchainImage(UI)",
                xrReleaseSwapchainImage(app->panels.ui_swapchain, &release));
}

static void *panel_worker(void *argument)
{
    struct app *app = argument;
    struct panel_thread *panel = &app->panels.panel_thread;
    pthread_mutex_lock(&panel->mutex);
    for (;;) {
        while (!panel->stopping && !panel->pending)
            pthread_cond_wait(&panel->changed, &panel->mutex);
        if (panel->stopping)
            break;
        panel->pending = false;
        panel->busy = true;
        pthread_mutex_unlock(&panel->mutex);
        pthread_mutex_lock(&app->panels.player_ui_mutex);
        player_ui_render(&app->panels.player_ui, &panel->job.state,
                         panel->pixels, app->panels.ui_bgra);
        pthread_mutex_unlock(&app->panels.player_ui_mutex);
        pthread_mutex_lock(&panel->mutex);
        panel->busy = false;
        panel->done = true;
    }
    pthread_mutex_unlock(&panel->mutex);
    return NULL;
}

void start_panel_thread(struct app *app)
{
    struct panel_thread *panel = &app->panels.panel_thread;
    pthread_mutex_init(&app->panels.player_ui_mutex, NULL);
    pthread_mutex_init(&panel->mutex, NULL);
    pthread_cond_init(&panel->changed, NULL);
    panel->pixels = calloc(1, UI_BYTES);
    need(panel->pixels != NULL, "allocate panel pixels");
    panel->started =
        pthread_create(&panel->thread, NULL, panel_worker, app) == 0;
    need(panel->started, "start panel thread");
}

void stop_panel_thread(struct app *app)
{
    struct panel_thread *panel = &app->panels.panel_thread;
    if (!panel->started)
        return;
    pthread_mutex_lock(&panel->mutex);
    panel->stopping = true;
    pthread_cond_signal(&panel->changed);
    pthread_mutex_unlock(&panel->mutex);
    pthread_join(panel->thread, NULL);
    panel->started = false;
    for (int i = 0; i < PLAYER_UI_PREVIEW_STRIP; ++i)
        ui_image_free(panel->job.preview[i]);
    free(panel->pixels);
}

static struct ui_image *copy_ui_image(const struct ui_image *image)
{
    struct ui_image *copy = ui_image_create(image->width, image->height);
    if (copy)
        memcpy(copy->rgba, image->rgba,
               (size_t)image->width * (size_t)image->height * 4U);
    return copy;
}

/* The panel's state with copies of what it points to, for the thread. */
static void fill_panel_job(const struct app *app, struct panel_job *job)
{
    for (int i = 0; i < PLAYER_UI_PREVIEW_STRIP; ++i) {
        ui_image_free(job->preview[i]);
        job->preview[i] = NULL;
    }
    struct player_ui_state state = player_state(app);
    snprintf(job->title, sizeof(job->title), "%s",
             state.title ? state.title : "");
    state.title = job->title;
    if (state.details) {
        job->details = *state.details;
        state.details = &job->details;
    }
    memcpy(job->adjust, app->adjust, sizeof(job->adjust));
    state.adjust = job->adjust;
    snprintf(job->update_installed, sizeof(job->update_installed), "%s",
             state.update_installed ? state.update_installed : "");
    snprintf(job->update_latest, sizeof(job->update_latest), "%s",
             state.update_latest ? state.update_latest : "");
    snprintf(job->update_status, sizeof(job->update_status), "%s",
             state.update_status ? state.update_status : "");
    state.update_installed = job->update_installed;
    state.update_latest = job->update_latest;
    state.update_status = job->update_status;
    memcpy(job->bindings, app->controls.bindings, sizeof(job->bindings));
    state.bindings = job->bindings;
    if (state.debug_text) {
        snprintf(job->debug_text, sizeof(job->debug_text), "%s",
                 state.debug_text);
        state.debug_text = job->debug_text;
    }
    for (int i = 0; i < PLAYER_UI_PREVIEW_STRIP; ++i)
        if (state.preview[i]) {
            job->preview[i] = copy_ui_image(state.preview[i]);
            state.preview[i] = job->preview[i];
        }
    job->state = state;
}

/* Per frame while playing: uploads a panel the thread finished and hands
 * it the next one when the state changed. */
void sync_player_panel(struct app *app)
{
    struct panel_thread *panel = &app->panels.panel_thread;
    pthread_mutex_lock(&panel->mutex);
    bool idle = !panel->busy && !panel->pending;
    bool done = idle && panel->done;
    panel->done = panel->done && !done;
    pthread_mutex_unlock(&panel->mutex);
    if (done) {
        double uploading = monotonic_seconds();
        upload_ui(app, panel->pixels);
        timing_ui(app, uploading);
    }
    if (!idle || !app->ui_dirty)
        return;
    fill_panel_job(app, &panel->job);
    app->ui_dirty = false;
    pthread_mutex_lock(&panel->mutex);
    panel->pending = true;
    pthread_cond_signal(&panel->changed);
    pthread_mutex_unlock(&panel->mutex);
}

/* Puts the pointer layer on the panel plane at the hit point, slightly in
 * front of the panel. */
void place_pointer(struct app *app, XrVector3f center, float yaw,
                   float width_meters, float height_meters,
                   struct panel_hit hit, float size)
{
    XrQuaternionf orientation = yaw_quaternion(yaw);
    XrVector3f local = {(hit.u - 0.5f) * width_meters,
                        (0.5f - hit.v) * height_meters, 0.006f};
    XrVector3f offset = rotate_vector(orientation, local);
    app->panels.pointer_pose = (XrPosef){
        orientation,
        {center.x + offset.x, center.y + offset.y, center.z + offset.z}};
    app->panels.pointer_size = size;
    app->panels.pointer_layer_visible = true;
}

XrCompositionLayerQuad make_ui_layer(const struct app *app)
{
    return (XrCompositionLayerQuad){
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->xr.space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = app->panels.ui_swapchain,
            .imageRect = {.extent = {UI_WIDTH, UI_HEIGHT}},
        },
        .pose = {yaw_quaternion(app->panel_yaw),
                 {app->panel_x, app->panel_y, app->panel_z}},
        .size = {UI_PANEL_WIDTH_METERS, UI_PANEL_HEIGHT_METERS},
    };
}

/* The browser stands in front of home at eye height. */
XrVector3f browser_center(const struct app *app)
{
    return (XrVector3f){
        app->home_position.x - sinf(app->home_yaw) * BROWSER_PANEL_DISTANCE,
        app->home_position.y,
        app->home_position.z - cosf(app->home_yaw) * BROWSER_PANEL_DISTANCE,
    };
}

void create_browser_texture(struct app *app)
{
    app->panels.browser_swapchain = create_ui_texture(
        app, &app->panels.browser_texture, BROWSER_UI_WIDTH, BROWSER_UI_HEIGHT,
        "xrCreateSwapchain(browser)");
    if (app->panels.browser_swapchain) {
        need_xr("xrEnumerateSwapchainImages(browser count)",
                xrEnumerateSwapchainImages(app->panels.browser_swapchain, 0,
                                           &app->panels.browser_image_count,
                                           NULL));
        need(app->panels.browser_image_count > 0 &&
                 app->panels.browser_image_count <= MAX_IMAGES,
             "unexpected browser swapchain image count");
        for (uint32_t i = 0; i < app->panels.browser_image_count; ++i)
            app->panels.browser_images[i].type =
                XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        need_xr("xrEnumerateSwapchainImages(browser list)",
                xrEnumerateSwapchainImages(
                    app->panels.browser_swapchain,
                    app->panels.browser_image_count,
                    &app->panels.browser_image_count,
                    (XrSwapchainImageBaseHeader *)app->panels.browser_images));
    }
    VkCommandBufferAllocateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->render.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    need_vk("vkAllocateCommandBuffers(browser)",
            vkAllocateCommandBuffers(app->gpu.device, &commands,
                                     &app->panels.browser_command));
    VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    need_vk("vkCreateFence(browser)",
            vkCreateFence(app->gpu.device, &fence, NULL,
                          &app->panels.browser_fence));
    const VkDeviceSize bytes = (VkDeviceSize)BROWSER_UI_WIDTH *
                               BROWSER_UI_HEIGHT * 4U;
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    need_vk("vkCreateBuffer(browser staging)",
            vkCreateBuffer(app->gpu.device, &buffer, NULL,
                           &app->panels.browser_staging_buffer));
    VkMemoryRequirements memory;
    vkGetBufferMemoryRequirements(app->gpu.device,
                                  app->panels.browser_staging_buffer, &memory);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory.size,
        .memoryTypeIndex = memory_type(
            app->gpu.physical, memory.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    need_vk("vkAllocateMemory(browser staging)",
            vkAllocateMemory(app->gpu.device, &allocation, NULL,
                             &app->panels.browser_staging_memory));
    need_vk("vkBindBufferMemory(browser staging)",
            vkBindBufferMemory(app->gpu.device,
                               app->panels.browser_staging_buffer,
                               app->panels.browser_staging_memory, 0));
    need_vk("vkMapMemory(browser staging)",
            vkMapMemory(app->gpu.device, app->panels.browser_staging_memory, 0,
                        bytes, 0, (void **)&app->panels.browser_pixels));
    app->panels.browser_cpu = calloc(1, (size_t)bytes);
    need(app->panels.browser_cpu != NULL, "allocate browser pixels");
    app->browser.dirty = true;
}

/* Uploads `pixels` (width x height RGBA, canvas layout) into every image of
 * a swapchain whose content never changes. */
void fill_static_swapchain(struct app *app, XrSwapchain swapchain,
                           const uint8_t *pixels, uint32_t width,
                           uint32_t height)
{
    uint32_t count = 0;
    XrSwapchainImageVulkan2KHR images[MAX_IMAGES];
    need_xr("xrEnumerateSwapchainImages(static count)",
            xrEnumerateSwapchainImages(swapchain, 0, &count, NULL));
    need(count > 0 && count <= MAX_IMAGES, "unexpected static image count");
    for (uint32_t i = 0; i < count; ++i)
        images[i] = (XrSwapchainImageVulkan2KHR){
            .type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};
    need_xr("xrEnumerateSwapchainImages(static list)",
            xrEnumerateSwapchainImages(swapchain, count, &count,
                                       (XrSwapchainImageBaseHeader *)images));
    VkDeviceSize bytes = (VkDeviceSize)width * height * 4U;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer;
    need_vk("vkCreateBuffer(static)",
            vkCreateBuffer(app->gpu.device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(app->gpu.device, buffer, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(
            app->gpu.physical, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory memory;
    need_vk("vkAllocateMemory(static)",
            vkAllocateMemory(app->gpu.device, &allocation, NULL, &memory));
    need_vk("vkBindBufferMemory(static)",
            vkBindBufferMemory(app->gpu.device, buffer, memory, 0));
    void *mapping;
    need_vk("vkMapMemory(static)",
            vkMapMemory(app->gpu.device, memory, 0, bytes, 0, &mapping));
    memcpy(mapping, pixels, (size_t)bytes);
    VkCommandBufferAllocateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->render.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command;
    need_vk("vkAllocateCommandBuffers(static)",
            vkAllocateCommandBuffers(app->gpu.device, &commands, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    need_vk("vkCreateFence(static)",
            vkCreateFence(app->gpu.device, &fence_info, NULL, &fence));
    for (uint32_t n = 0; n < count; ++n) {
        XrSwapchainImageAcquireInfo acquire = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t index = 0;
        need_xr("xrAcquireSwapchainImage(static)",
                xrAcquireSwapchainImage(swapchain, &acquire, &index));
        XrSwapchainImageWaitInfo wait = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
            .timeout = XR_INFINITE_DURATION};
        need_xr("xrWaitSwapchainImage(static)",
                xrWaitSwapchainImage(swapchain, &wait));
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        need_vk("vkBeginCommandBuffer(static)",
                vkBeginCommandBuffer(command, &begin));
        VkImageMemoryBarrier transfer = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = images[index].image,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .levelCount = 1, .layerCount = 1},
        };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &transfer);
        VkBufferImageCopy copy = {
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .layerCount = 1},
            .imageExtent = {width, height, 1},
        };
        vkCmdCopyBufferToImage(command, buffer, images[index].image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImageMemoryBarrier sample = transfer;
        sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                             0, NULL, 1, &sample);
        need_vk("vkEndCommandBuffer(static)", vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                               .commandBufferCount = 1,
                               .pCommandBuffers = &command};
        need_vk("vkQueueSubmit(static)",
                vkQueueSubmit(app->gpu.queue, 1, &submit, fence));
        need_vk("vkWaitForFences(static)",
                vkWaitForFences(app->gpu.device, 1, &fence, VK_TRUE,
                                UINT64_C(5000000000)));
        need_vk("vkResetFences(static)",
                vkResetFences(app->gpu.device, 1, &fence));
        need_vk("vkResetCommandBuffer(static)",
                vkResetCommandBuffer(command, 0));
        XrSwapchainImageReleaseInfo release = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        need_xr("xrReleaseSwapchainImage(static)",
                xrReleaseSwapchainImage(swapchain, &release));
    }
    vkDestroyFence(app->gpu.device, fence, NULL);
    vkFreeCommandBuffers(app->gpu.device, app->render.command_pool, 1,
                         &command);
    vkUnmapMemory(app->gpu.device, memory);
    vkDestroyBuffer(app->gpu.device, buffer, NULL);
    vkFreeMemory(app->gpu.device, memory, NULL);
}

/* Pipeline for the panels drawn into the eye images: a textured quad,
 * blended over the video with premultiplied alpha like a quad layer. */
void create_panel_pipeline(struct app *app)
{
    if (!app->panels.panels_in_eye_images)
        return;
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .anisotropyEnable = app->gpu.panel_anisotropy > 1.0f,
        .maxAnisotropy = app->gpu.panel_anisotropy,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    need_vk("vkCreateSampler(panel)",
            vkCreateSampler(app->gpu.device, &sampler, NULL,
                            &app->panels.panel_sampler));
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = &app->panels.panel_sampler,
    };
    VkDescriptorSetLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding,
    };
    need_vk("vkCreateDescriptorSetLayout(panel)",
            vkCreateDescriptorSetLayout(app->gpu.device, &layout, NULL,
                                        &app->panels.panel_descriptor_layout));
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .size = sizeof(struct mat4),
    };
    VkPipelineLayoutCreateInfo pipeline_layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &app->panels.panel_descriptor_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    need_vk("vkCreatePipelineLayout(panel)",
            vkCreatePipelineLayout(app->gpu.device, &pipeline_layout, NULL,
                                   &app->panels.panel_pipeline_layout));
    VkDescriptorPoolSize size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 2,
    };
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &size,
    };
    need_vk("vkCreateDescriptorPool(panel)",
            vkCreateDescriptorPool(app->gpu.device, &pool, NULL,
                                   &app->panels.panel_descriptor_pool));
    struct ui_texture *textures[2] = {&app->panels.ui_texture,
                                      &app->panels.browser_texture};
    for (size_t i = 0; i < ARRAY_COUNT(textures); ++i) {
        VkDescriptorSetAllocateInfo allocate = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = app->panels.panel_descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &app->panels.panel_descriptor_layout,
        };
        need_vk("vkAllocateDescriptorSets(panel)",
                vkAllocateDescriptorSets(app->gpu.device, &allocate,
                                         &textures[i]->descriptor_set));
        VkDescriptorImageInfo image = {
            .imageView = textures[i]->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = textures[i]->descriptor_set, .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image,
        };
        vkUpdateDescriptorSets(app->gpu.device, 1, &write, 0, NULL);
    }
    VkShaderModule vert = shader(app->gpu.device, "ui-panel.vert.spv");
    VkShaderModule frag = shader(app->gpu.device, "ui-panel.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag,
         .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    VkViewport viewport = {.width = (float)app->overlay.width,
                           .height = (float)app->overlay.height,
                           .maxDepth = 1};
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
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .blendEnable = VK_TRUE,
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
    /* Drawn first in the overlay; the controllers come over it. */
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };
    VkGraphicsPipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
        .pInputAssemblyState = &assembly, .pViewportState = &view,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pDepthStencilState = &depth, .pColorBlendState = &blend,
        .layout = app->panels.panel_pipeline_layout,
        .renderPass = app->overlay.pass,
    };
    need_vk("vkCreateGraphicsPipelines(panel)",
            vkCreateGraphicsPipelines(app->gpu.device, VK_NULL_HANDLE, 1,
                                      &pipeline, NULL,
                                      &app->panels.panel_pipeline));
    vkDestroyShaderModule(app->gpu.device, vert, NULL);
    vkDestroyShaderModule(app->gpu.device, frag, NULL);
}

/* Model matrix of a panel quad (unit square in the shader) with the pose
 * and size of its quad layer. */
struct mat4 panel_model(const XrCompositionLayerQuad *quad)
{
    struct mat4 model = rotation_from_pose(quad->pose);
    for (unsigned row = 0; row < 3; ++row) {
        model.v[row] *= quad->size.width;
        model.v[4 + row] *= quad->size.height;
    }
    model.v[12] = quad->pose.position.x;
    model.v[13] = quad->pose.position.y;
    model.v[14] = quad->pose.position.z;
    return model;
}

void create_pointer_texture(struct app *app)
{
    XrSwapchainCreateInfo info = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
        .format = app->panels.ui_format,
        .sampleCount = 1,
        .width = POINTER_PIXELS,
        .height = POINTER_PIXELS,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };
    need_xr("xrCreateSwapchain(pointer)",
            xrCreateSwapchain(app->xr.session, &info,
                              &app->panels.pointer_swapchain));
    uint8_t pixels[POINTER_PIXELS * POINTER_PIXELS * 4] = {0};
    struct ui_canvas canvas = {pixels, POINTER_PIXELS, POINTER_PIXELS,
                               app->panels.ui_bgra};
    const float c = POINTER_PIXELS / 2.0f;
    ui_fill_circle(&canvas, c, c, c - 2, UI_RGBA(0, 0, 0, 110));
    ui_fill_circle(&canvas, c, c, c - 8, UI_RGBA(255, 255, 255, 240));
    ui_fill_circle(&canvas, c, c, c - 17, UI_RGBA(56, 160, 255, 255));
    fill_static_swapchain(app, app->panels.pointer_swapchain, pixels,
                          POINTER_PIXELS, POINTER_PIXELS);
}

XrCompositionLayerQuad make_pointer_layer(const struct app *app)
{
    return (XrCompositionLayerQuad){
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->xr.space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = app->panels.pointer_swapchain,
            .imageRect = {.extent = {POINTER_PIXELS, POINTER_PIXELS}},
        },
        .pose = app->panels.pointer_pose,
        .size = {app->panels.pointer_size, app->panels.pointer_size},
    };
}

void upload_browser(struct app *app)
{
    /* Draw in cached memory: blending reads pixels back, which is very slow
     * on the uncached staging mapping. */
    browser_ui_render(&app->browser, app->panels.browser_cpu,
                      app->panels.ui_bgra);
    if (app->settings_open)
        compose_settings(app);
    XrSwapchainImageAcquireInfo acquire = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
    };
    uint32_t index = 0;
    XrSwapchainImageWaitInfo wait = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = XR_INFINITE_DURATION,
    };
    if (app->panels.browser_swapchain) {
        need_xr("xrAcquireSwapchainImage(browser)",
                xrAcquireSwapchainImage(app->panels.browser_swapchain, &acquire, &index));
        need_xr("xrWaitSwapchainImage(browser)",
                xrWaitSwapchainImage(app->panels.browser_swapchain, &wait));
        need(index < app->panels.browser_image_count, "invalid browser swapchain index");
    }
    need_vk("vkWaitForFences(browser)",
            vkWaitForFences(app->gpu.device, 1, &app->panels.browser_fence,
                            VK_TRUE, UINT64_C(5000000000)));
    need_vk("vkResetFences(browser)",
            vkResetFences(app->gpu.device, 1, &app->panels.browser_fence));
    memcpy(app->panels.browser_pixels, app->panels.browser_cpu,
           (size_t)BROWSER_UI_WIDTH * BROWSER_UI_HEIGHT * 4U);
    need_vk("vkResetCommandBuffer(browser)",
            vkResetCommandBuffer(app->panels.browser_command, 0));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    need_vk("vkBeginCommandBuffer(browser)",
            vkBeginCommandBuffer(app->panels.browser_command, &begin));
    record_ui_upload(app->panels.browser_command, &app->panels.browser_texture,
                     app->panels.browser_images[index].image,
                     app->panels.browser_staging_buffer,
                     app->panels.browser_image_initialized[index]);
    need_vk("vkEndCommandBuffer(browser)",
            vkEndCommandBuffer(app->panels.browser_command));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &app->panels.browser_command,
    };
    /* SteamVR samples the image as soon as it is released, so the copy must
     * be complete by then (otherwise it shows stale or empty content). */
    need_vk(
        "vkQueueSubmit(browser)",
        vkQueueSubmit(app->gpu.queue, 1, &submit, app->panels.browser_fence));
    need_vk("vkWaitForFences(browser completion)",
            vkWaitForFences(app->gpu.device, 1, &app->panels.browser_fence,
                            VK_TRUE, UINT64_C(5000000000)));
    app->panels.browser_image_initialized[index] = true;
    XrSwapchainImageReleaseInfo release = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
    };
    if (app->panels.browser_swapchain)
        need_xr(
            "xrReleaseSwapchainImage(browser)",
            xrReleaseSwapchainImage(app->panels.browser_swapchain, &release));
}

XrCompositionLayerQuad make_browser_layer(const struct app *app)
{
    return (XrCompositionLayerQuad){
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->xr.space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = app->panels.browser_swapchain,
            .imageRect = {.extent = {BROWSER_UI_WIDTH, BROWSER_UI_HEIGHT}},
        },
        .pose = {yaw_quaternion(app->home_yaw), browser_center(app)},
        .size = {BROWSER_UI_PANEL_WIDTH_METERS,
                 BROWSER_UI_PANEL_HEIGHT_METERS},
    };
}

bool panel_hit_inside(const struct panel_hit *hit)
{
    return hit->plane && hit->u >= 0.0f && hit->u < 1.0f &&
           hit->v >= 0.0f && hit->v < 1.0f;
}

/* Intersect the aim ray with the (unbounded) panel plane. u/v are panel
 * texture coordinates and may lie outside [0,1). */
struct panel_hit panel_ray_at(XrVector3f center, float yaw,
                              float width_meters, float height_meters,
                              XrPosef aim)
{
    struct panel_hit hit = {0};
    XrQuaternionf orientation = yaw_quaternion(yaw);
    XrVector3f normal = rotate_vector(orientation, (XrVector3f){0, 0, 1});
    XrVector3f direction =
        rotate_vector(aim.orientation, (XrVector3f){0, 0, -1});
    float denominator = vector_dot(direction, normal);
    if (denominator > -0.0001f)
        return hit;
    XrVector3f to_center = {center.x - aim.position.x,
                            center.y - aim.position.y,
                            center.z - aim.position.z};
    float distance = vector_dot(to_center, normal) / denominator;
    if (distance <= 0.0f)
        return hit;
    XrVector3f point = {aim.position.x + direction.x * distance - center.x,
                        aim.position.y + direction.y * distance - center.y,
                        aim.position.z + direction.z * distance - center.z};
    XrVector3f local = rotate_vector(quaternion_conjugate(orientation), point);
    hit.plane = true;
    hit.distance = distance;
    hit.u = local.x / width_meters + 0.5f;
    hit.v = 0.5f - local.y / height_meters;
    return hit;
}

/* Intersect the aim ray with the (unbounded) player panel plane. u/v are
 * panel texture coordinates and may lie outside [0,1). */
struct panel_hit panel_ray(const struct app *app, XrPosef aim)
{
    return panel_ray_at((XrVector3f){app->panel_x, app->panel_y, app->panel_z},
                        app->panel_yaw, UI_PANEL_WIDTH_METERS,
                        UI_PANEL_HEIGHT_METERS, aim);
}

/* The visible panel of this frame: its quad layer and, drawn into the
 * eye images, its texture and model matrix. */
void place_panels(struct app *app, struct frame *f)
{
    f->ui_layer = app->playing
                                          ? make_ui_layer(app)
                                          : make_browser_layer(app);
    f->show_ui = app->playing ? app->ui_visible : true;
    f->panel = NULL;
    if (app->panels.panels_in_eye_images && f->show_ui)
        f->panel = app->playing ? &app->panels.ui_texture
                                : &app->panels.browser_texture;
    f->panel_matrix = panel_model(&f->ui_layer);
}
