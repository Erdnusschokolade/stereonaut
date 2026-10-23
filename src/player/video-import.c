#define _POSIX_C_SOURCE 200809L

#include "renderer.h"

#define DRM_FORMAT_MOD_LINEAR UINT64_C(0)
#define DRM_FORMAT_MOD_QCOM_COMPRESSED UINT64_C(0x0500000000000001)

static void apply_init(struct app *app, const struct nv12_ipc_message *message,
                       const struct msghdr *received)
{
    /* Iris chooses the count per stream (H.264 needs more references at
     * small sizes), so it may differ between titles and seeks. */
    need(message->buffer_count >= 1 &&
             message->buffer_count <= NV12_IPC_MAX_BUFFERS,
         "NV12 IPC buffer count out of range");
    struct cmsghdr *header = CMSG_FIRSTHDR(received);
    need(header && header->cmsg_level == SOL_SOCKET &&
             header->cmsg_type == SCM_RIGHTS,
         "NV12 IPC init lacks DMA-BUF file descriptors");
    size_t fd_bytes = header->cmsg_len - CMSG_LEN(0);
    need(fd_bytes == message->buffer_count * sizeof(int),
         "NV12 IPC descriptor count differs from buffer count");
    int *fds = (int *)CMSG_DATA(header);
    app->video.buffer_count = message->buffer_count;
    for (uint32_t i = 0; i < app->video.buffer_count; ++i) {
        app->video.buffers[i].dma_buf_fd = fds[i];
        struct stat status;
        if (fstat(fds[i], &status) < 0) {
            perror("fstat IPC DMA-BUF");
            exit(2);
        }
        need((uint64_t)status.st_size == message->size,
             "IPC DMA-BUF length differs from contract");
    }
    app->video.width = message->width;
    app->video.height = message->height;
    app->video.stride = message->stride;
    app->video.dma_buf_size = message->size;
    app->video.uv_offset = message->uv_offset;
    app->video.format = message->pixel_format;
    app->video.buffer_kind = message->buffer_kind;
    need(app->video.buffer_kind == NV12_BUFFER_DMA_BUF ||
             (app->video.buffer_kind == NV12_BUFFER_HOST &&
              app->gpu.get_host_pointer_properties),
         "video buffers in host memory need VK_EXT_external_memory_host");
    app->video.coded_width = message->coded_width;
    app->video.coded_height = message->coded_height;
    need(app->video.format == NV12_FORMAT_NV12 ||
             (app->video.format == NV12_FORMAT_P010 &&
              app->video.coded_width == app->video.width &&
              app->video.coded_height == app->video.height &&
              app->video.stride >= 2U * app->video.width) ||
             (app->video.format == NV12_FORMAT_TP10 &&
              app->video.coded_width >= app->video.width &&
              app->video.coded_height >= app->video.height &&
              app->video.coded_width % 3U == 0 &&
              app->video.stride == app->video.coded_width / 3U * 4U),
         "unsupported video pixel format in IPC contract");
    app->projection_mode = message->projection_mode;
    app->stereo_layout = message->stereo_layout;
    app->swap_eyes = message->swap_eyes != 0;
    app->video_levels = message->video_levels;
    app->format_source = message->format_source;
    app->fisheye_fov = message->fisheye_fov >= 120 &&
                               message->fisheye_fov <= 240
                           ? message->fisheye_fov
                           : NV12_FISHEYE_FOV_DEFAULT;
    need(app->video.width > 0 && app->video.height > 0 &&
             !(app->video.width & 1U) && !(app->video.height & 1U) &&
             (app->video.format == NV12_FORMAT_TP10 ||
              app->video.stride >= app->video.width) &&
             app->video.uv_offset >=
                 (uint64_t)app->video.stride * app->video.height &&
             app->video.dma_buf_size >= app->video.uv_offset +
                 (uint64_t)app->video.stride * app->video.height / 2U,
         "NV12 IPC layout is not linear 4:2:0");
    need(app->projection_mode <= NV12_PROJECTION_FISHEYE,
         "invalid projection mode in IPC contract");
    need(app->stereo_layout <= NV12_STEREO_TB,
         "invalid stereo layout in IPC contract");
    need(app->video_levels <= NV12_LEVELS_FULL,
         "invalid video levels in IPC contract");
    need(app->format_source <= NV12_FORMAT_SOURCE_USER,
         "invalid format source in IPC contract");
    /* The host owns the panel placement for its whole lifetime. */
    printf("render_config projection=%u stereo=%u swap_eyes=%u levels=%u"
           " view0=left view1=right\n",
           app->projection_mode, app->stereo_layout, app->swap_eyes,
           app->video_levels);
}

/* One plane of a two-plane frame as its own image, sampled by the plane
 * shader (tp10-projection.frag) instead of a YCbCr sampler.
 * TP10 (Iris Main10): TP10 packs three 10-bit samples per word, so a plane
 * has the byte geometry (and Turnip's UBWC layout) of an RGBA8 image a
 * third as wide. The patched Turnip samples such views as FMT6_TP10_Y
 * (RGBA8) or FMT6_TP10_UV (RGB10A2) when STEREONAUT_TP10 is set
 * (tools/mesa-tp10).
 * P010 (PC hardware decoders): linear R16 luma and RG16 chroma, the 10 bits
 * in the high bits, which the shader reads like TP10's normalized codes. */
static void import_plane(struct app *app, struct video_buffer *buffer,
                         VkFormat format, uint64_t drm_modifier,
                         uint32_t width, VkDeviceSize offset, uint32_t rows,
                         VkImage *image, VkDeviceMemory *memory,
                         VkImageView *view)
{
    const VkSubresourceLayout plane = {
        .offset = offset, .rowPitch = app->video.stride,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = drm_modifier,
        .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &plane,
    };
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {width, rows, 1},
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    need_vk("vkCreateImage(video plane)",
            vkCreateImage(app->gpu.device, &info, NULL, image));
    VkImageDrmFormatModifierPropertiesEXT chosen = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
    };
    need_vk("vkGetImageDrmFormatModifierPropertiesEXT(plane)",
            app->gpu.get_modifier_properties(app->gpu.device, *image, &chosen));
    need(chosen.drmFormatModifier == drm_modifier,
         "Vulkan substituted the video plane modifier");
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(app->gpu.device, *image, &requirements);
    need(requirements.size <= app->video.dma_buf_size,
         "video plane layout exceeds the DMA-BUF");
    VkMemoryFdPropertiesKHR fd_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    need_vk("vkGetMemoryFdPropertiesKHR(plane)",
            app->gpu.get_memory_fd_properties(
                app->gpu.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                buffer->dma_buf_fd, &fd_properties));
    uint32_t bits = requirements.memoryTypeBits & fd_properties.memoryTypeBits;
    need(bits != 0, "plane DMA-BUF memory-type intersection is empty");
    int imported_fd = dup(buffer->dma_buf_fd);
    if (imported_fd < 0) { perror("dup plane DMA-BUF"); exit(2); }
    VkImportMemoryFdInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = imported_fd,
    };
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import, .image = *image,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated, .allocationSize = app->video.dma_buf_size,
        .memoryTypeIndex = memory_type(app->gpu.physical, bits, 0),
    };
    need_vk("vkAllocateMemory(import plane)",
            vkAllocateMemory(app->gpu.device, &allocation, NULL, memory));
    need_vk("vkBindImageMemory(import plane)",
            vkBindImageMemory(app->gpu.device, *image, *memory, 0));
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    need_vk("vkCreateImageView(plane)",
            vkCreateImageView(app->gpu.device, &view_info, NULL, view));
}

/* A device-local image the host buffer is copied into: NV12 through the
 * YCbCr sampler, or one P010 plane for the plane shader. */
static void create_upload_image(struct app *app, VkFormat format,
                                uint32_t width, uint32_t height, bool ycbcr,
                                VkImage *image, VkDeviceMemory *memory,
                                VkImageView *view)
{
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    need_vk("vkCreateImage(upload)",
            vkCreateImage(app->gpu.device, &info, NULL, image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(app->gpu.device, *image, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(app->gpu.physical,
                                       requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    need_vk("vkAllocateMemory(upload)",
            vkAllocateMemory(app->gpu.device, &allocation, NULL, memory));
    need_vk("vkBindImageMemory(upload)",
            vkBindImageMemory(app->gpu.device, *image, *memory, 0));
    VkSamplerYcbcrConversionInfo conversion = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = app->video.conversion,
    };
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = ycbcr ? &conversion : NULL,
        .image = *image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    need_vk("vkCreateImageView(upload)",
            vkCreateImageView(app->gpu.device, &view_info, NULL, view));
}

/* NV12_BUFFER_HOST: map the memfd, import the mapping as host memory of a
 * transfer buffer and create the images it is copied into. Sampling
 * device-local images is also much cheaper than reading system memory over
 * PCIe for every eye and display frame. */
static void import_host_buffer(struct app *app, uint32_t i)
{
    double started = monotonic_seconds();
    struct video_buffer *buffer = &app->video.buffers[i];
    bool p010 = app->video.format == NV12_FORMAT_P010;
    buffer->host = true;
    buffer->tp10 = p010;
    buffer->host_size = (size_t)app->video.dma_buf_size;
    void *mapping = mmap(NULL, buffer->host_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, buffer->dma_buf_fd, 0);
    need(mapping != MAP_FAILED, "map host video buffer");
    buffer->host_mapping = mapping;
    VkExternalMemoryBufferCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
    };
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external,
        .size = app->video.dma_buf_size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    need_vk("vkCreateBuffer(host video)",
            vkCreateBuffer(app->gpu.device, &buffer_info, NULL,
                           &buffer->host_buffer));
    VkMemoryHostPointerPropertiesEXT pointer = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
    };
    need_vk("vkGetMemoryHostPointerPropertiesEXT",
            app->gpu.get_host_pointer_properties(
                app->gpu.device,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                mapping, &pointer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(app->gpu.device, buffer->host_buffer,
                                  &requirements);
    uint32_t bits = requirements.memoryTypeBits & pointer.memoryTypeBits;
    need(bits != 0, "host video memory-type intersection is empty");
    VkImportMemoryHostPointerInfoEXT import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = mapping,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
        .allocationSize = app->video.dma_buf_size,
        .memoryTypeIndex = memory_type(app->gpu.physical, bits, 0),
    };
    need_vk("vkAllocateMemory(import host video)",
            vkAllocateMemory(app->gpu.device, &allocation, NULL,
                             &buffer->host_memory));
    need_vk("vkBindBufferMemory(host video)",
            vkBindBufferMemory(app->gpu.device, buffer->host_buffer,
                               buffer->host_memory, 0));
    if (p010) {
        create_upload_image(app, VK_FORMAT_R16_UNORM, app->video.width,
                            app->video.height, false, &buffer->image,
                            &buffer->memory, &buffer->view);
        create_upload_image(app, VK_FORMAT_R16G16_UNORM,
                            app->video.width / 2U, app->video.height / 2U,
                            false, &buffer->image_uv, &buffer->memory_uv,
                            &buffer->view_uv);
    } else {
        create_upload_image(app, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
                            app->video.width, app->video.height, true,
                            &buffer->image, &buffer->memory, &buffer->view);
    }
    printf("host_import index=%u format=%s bytes=%zu ms=%.1f\n", i,
           p010 ? "p010" : "nv12", buffer->host_size,
           (monotonic_seconds() - started) * 1e3);
}

/* Records the copy of a new host-memory frame into its image(s); the
 * images stay in GENERAL like imported DMA-BUFs. */
void record_host_upload(struct app *app,
                        const struct video_buffer *video)
{
    uint32_t count = video->tp10 ? 2U : 1U;
    VkImageMemoryBarrier barriers[2];
    for (uint32_t i = 0; i < count; ++i)
        barriers[i] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = i ? video->image_uv : video->image,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .levelCount = 1, .layerCount = 1},
        };
    vkCmdPipelineBarrier(app->render.command,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         count, barriers);
    /* bufferRowLength counts texels of the plane's format. */
    uint32_t stride = app->video.stride;
    if (video->tp10) {
        VkBufferImageCopy luma = {
            .bufferRowLength = stride / 2U,
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .layerCount = 1},
            .imageExtent = {app->video.width, app->video.height, 1},
        };
        VkBufferImageCopy chroma = {
            .bufferOffset = app->video.uv_offset, .bufferRowLength = stride / 4U,
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .layerCount = 1},
            .imageExtent = {app->video.width / 2U, app->video.height / 2U, 1},
        };
        vkCmdCopyBufferToImage(app->render.command, video->host_buffer, video->image,
                               VK_IMAGE_LAYOUT_GENERAL, 1, &luma);
        vkCmdCopyBufferToImage(app->render.command, video->host_buffer,
                               video->image_uv, VK_IMAGE_LAYOUT_GENERAL, 1,
                               &chroma);
    } else {
        VkBufferImageCopy planes[2] = {
            {.bufferRowLength = stride,
             .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
                                  .layerCount = 1},
             .imageExtent = {app->video.width, app->video.height, 1}},
            {.bufferOffset = app->video.uv_offset, .bufferRowLength = stride / 2U,
             .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
                                  .layerCount = 1},
             .imageExtent = {app->video.width / 2U, app->video.height / 2U,
                             1}},
        };
        vkCmdCopyBufferToImage(app->render.command, video->host_buffer, video->image,
                               VK_IMAGE_LAYOUT_GENERAL, 2, planes);
    }
    for (uint32_t i = 0; i < count; ++i) {
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    vkCmdPipelineBarrier(app->render.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, count, barriers);
}

static void import_video_buffer(struct app *app, uint32_t i)
{
    if (app->video.buffer_kind == NV12_BUFFER_HOST) {
        import_host_buffer(app, i);
        return;
    }
    struct video_buffer *tp10_buffer = &app->video.buffers[i];
    /* TP10 and P010 both go through the plane shader. */
    bool p010 = app->video.format == NV12_FORMAT_P010;
    tp10_buffer->tp10 = app->video.format == NV12_FORMAT_TP10 || p010;
    if (tp10_buffer->tp10) {
        uint64_t modifier = p010 ? DRM_FORMAT_MOD_LINEAR
                                 : DRM_FORMAT_MOD_QCOM_COMPRESSED;
        uint32_t width = p010 ? app->video.coded_width : app->video.coded_width / 3U;
        import_plane(app, tp10_buffer,
                     p010 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8G8B8A8_UNORM,
                     modifier, width, 0, app->video.coded_height,
                     &tp10_buffer->image, &tp10_buffer->memory,
                     &tp10_buffer->view);
        import_plane(app, tp10_buffer,
                     p010 ? VK_FORMAT_R16G16_UNORM
                          : VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                     modifier, p010 ? width / 2U : width, app->video.uv_offset,
                     (app->video.coded_height + 1U) / 2U,
                     &tp10_buffer->image_uv, &tp10_buffer->memory_uv,
                     &tp10_buffer->view_uv);
        VkSemaphoreCreateInfo semaphore = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        need_vk("vkCreateSemaphore(producer)",
                vkCreateSemaphore(app->gpu.device, &semaphore, NULL,
                                  &tp10_buffer->producer_semaphore));
        printf("%s_import index=%u coded=%ux%u\n", p010 ? "p010" : "tp10",
               i, app->video.coded_width, app->video.coded_height);
        return;
    }
    VkSamplerYcbcrConversionInfo conversion_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = app->video.conversion,
    };
    const VkSubresourceLayout planes[2] = {
        {.offset = 0,
         .size = (VkDeviceSize)app->video.stride * app->video.height,
         .rowPitch = app->video.stride},
        {.offset = app->video.uv_offset,
         .size = (VkDeviceSize)app->video.stride * app->video.height / 2U,
         .rowPitch = app->video.stride},
    };
    struct video_buffer *buffer = &app->video.buffers[i];
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
        .drmFormatModifierPlaneCount = 2, .pPlaneLayouts = planes,
    };
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .extent = {app->video.width, app->video.height, 1},
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    need_vk("vkCreateImage(NV12 DMA-BUF)",
            vkCreateImage(app->gpu.device, &image, NULL, &buffer->image));
    VkImageDrmFormatModifierPropertiesEXT chosen = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
    };
    need_vk("vkGetImageDrmFormatModifierPropertiesEXT",
            app->gpu.get_modifier_properties(app->gpu.device, buffer->image,
                                         &chosen));
    need(chosen.drmFormatModifier == DRM_FORMAT_MOD_LINEAR,
         "Vulkan substituted the NV12 modifier");
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(app->gpu.device, buffer->image, &requirements);
    need(requirements.size <= app->video.dma_buf_size,
         "NV12 Vulkan memory requirement exceeds DMA-BUF");
    VkMemoryFdPropertiesKHR fd_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    need_vk("vkGetMemoryFdPropertiesKHR",
            app->gpu.get_memory_fd_properties(
                app->gpu.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                buffer->dma_buf_fd, &fd_properties));
    uint32_t bits =
        requirements.memoryTypeBits & fd_properties.memoryTypeBits;
    need(bits != 0, "NV12 DMA-BUF memory-type intersection is empty");
    uint32_t type = memory_type(app->gpu.physical, bits, 0);
    int imported_fd = dup(buffer->dma_buf_fd);
    if (imported_fd < 0) { perror("dup NV12 DMA-BUF"); exit(2); }
    VkImportMemoryFdInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = imported_fd,
    };
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import, .image = buffer->image,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated, .allocationSize = app->video.dma_buf_size,
        .memoryTypeIndex = type,
    };
    need_vk("vkAllocateMemory(import NV12)",
            vkAllocateMemory(app->gpu.device, &allocation, NULL,
                             &buffer->memory));
    need_vk("vkBindImageMemory(import NV12)",
            vkBindImageMemory(app->gpu.device, buffer->image,
                              buffer->memory, 0));
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &conversion_info, .image = buffer->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    need_vk("vkCreateImageView(NV12)",
            vkCreateImageView(app->gpu.device, &view, NULL, &buffer->view));
    VkSemaphoreCreateInfo semaphore = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    need_vk("vkCreateSemaphore(producer)",
            vkCreateSemaphore(app->gpu.device, &semaphore, NULL,
                              &buffer->producer_semaphore));
    printf("nv12_import index=%u memory_type=%u\n", i, type);
}

/* Format check and the YCbCr sampler shared by every imported frame. The
 * sampler only reconstructs chroma (RGB_IDENTITY: r = Cr, g = Y, b = Cb);
 * the shader applies the title's levels and BT.709. */
void create_video_sampler(struct app *app)
{
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 format_properties = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list,
    };
    vkGetPhysicalDeviceFormatProperties2(
        app->gpu.physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        &format_properties);
    need(list.drmFormatModifierCount > 0, "NV12 exposes no DRM modifiers");
    list.pDrmFormatModifierProperties =
        calloc(list.drmFormatModifierCount,
               sizeof(*list.pDrmFormatModifierProperties));
    need(list.pDrmFormatModifierProperties != NULL,
         "allocate NV12 modifier properties");
    vkGetPhysicalDeviceFormatProperties2(
        app->gpu.physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        &format_properties);
    VkFormatFeatureFlags features = 0;
    for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i) {
        VkDrmFormatModifierPropertiesEXT *item =
            &list.pDrmFormatModifierProperties[i];
        if (item->drmFormatModifier == DRM_FORMAT_MOD_LINEAR &&
            item->drmFormatModifierPlaneCount == 2)
            features = item->drmFormatModifierTilingFeatures;
    }
    free(list.pDrmFormatModifierProperties);
    need(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
         "linear two-plane NV12 is not sampleable");
    need(features &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT,
         "linear-filtered NV12 YCbCr conversion is unsupported");
    VkChromaLocation chroma_location = VK_CHROMA_LOCATION_COSITED_EVEN;
    if (features & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT)
        chroma_location = VK_CHROMA_LOCATION_MIDPOINT;
    else if (features & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT)
        chroma_location = VK_CHROMA_LOCATION_COSITED_EVEN;
    else
        need(false, "NV12 exposes no supported chroma sample location");

    VkSamplerYcbcrConversionCreateInfo conversion = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY,
        .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY},
        .xChromaOffset = chroma_location,
        .yChromaOffset = chroma_location,
        .chromaFilter = VK_FILTER_LINEAR,
    };
    need_vk("vkCreateSamplerYcbcrConversion",
            vkCreateSamplerYcbcrConversion(app->gpu.device, &conversion, NULL,
                                           &app->video.conversion));
    VkSamplerYcbcrConversionInfo conversion_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = app->video.conversion,
    };
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = &conversion_info,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
    };
    need_vk("vkCreateSampler(NV12)",
            vkCreateSampler(app->gpu.device, &sampler, NULL,
                            &app->video.sampler));
    VkSamplerCreateInfo plane_sampler = sampler;
    plane_sampler.pNext = NULL;
    need_vk("vkCreateSampler(TP10)",
            vkCreateSampler(app->gpu.device, &plane_sampler, NULL,
                            &app->video.plane_sampler));

    printf("nv12_sampler format=%d model=rgb_identity levels=shader"
           " chroma_location=%s\n",
           VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
           chroma_location == VK_CHROMA_LOCATION_MIDPOINT ? "midpoint"
                                                          : "cosited_even");
}

static void write_video_descriptor(struct app *app, VkDescriptorSet set,
                                   const struct video_buffer *buffer)
{
    VkImageView view = buffer->view;
    VkDescriptorBufferInfo descriptor_buffer = {
        .buffer = app->render.uniform_buffer,
        .offset = 0,
        .range = sizeof(struct view_uniform),
    };
    VkDescriptorImageInfo descriptor_image = {
        .sampler = app->video.sampler,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkDescriptorImageInfo planes[2] = {
        {.sampler = app->video.plane_sampler, .imageView = buffer->view,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.sampler = app->video.plane_sampler, .imageView = buffer->view_uv,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
    };
    VkDescriptorBufferInfo disparity = {
        .buffer = app->autofocus.disparity_buffer, .offset = 0,
        .range = sizeof(struct disparity_result),
    };
    VkWriteDescriptorSet writes[4] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         .pBufferInfo = &descriptor_buffer},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &descriptor_image},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set, .dstBinding = 2, .descriptorCount = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = planes},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &disparity},
    };
    vkUpdateDescriptorSets(app->gpu.device, 1, &writes[3], 0, NULL);
    if (buffer->tp10) {
        vkUpdateDescriptorSets(app->gpu.device, 1, &writes[0], 0, NULL);
        vkUpdateDescriptorSets(app->gpu.device, 1, &writes[2], 0, NULL);
    } else {
        vkUpdateDescriptorSets(app->gpu.device, 2, writes, 0, NULL);
    }
}

void destroy_video_buffer(struct app *app, struct video_buffer *buffer)
{
    VkDescriptorSet set = buffer->descriptor_set;
    if (buffer->producer_semaphore)
        vkDestroySemaphore(app->gpu.device, buffer->producer_semaphore, NULL);
    if (buffer->view)
        vkDestroyImageView(app->gpu.device, buffer->view, NULL);
    if (buffer->image)
        vkDestroyImage(app->gpu.device, buffer->image, NULL);
    if (buffer->memory)
        vkFreeMemory(app->gpu.device, buffer->memory, NULL);
    if (buffer->view_uv)
        vkDestroyImageView(app->gpu.device, buffer->view_uv, NULL);
    if (buffer->image_uv)
        vkDestroyImage(app->gpu.device, buffer->image_uv, NULL);
    if (buffer->memory_uv)
        vkFreeMemory(app->gpu.device, buffer->memory_uv, NULL);
    if (buffer->host_buffer)
        vkDestroyBuffer(app->gpu.device, buffer->host_buffer, NULL);
    if (buffer->host_memory)
        vkFreeMemory(app->gpu.device, buffer->host_memory, NULL);
    if (buffer->host_mapping)
        munmap(buffer->host_mapping, buffer->host_size);
    if (buffer->dma_buf_fd >= 0)
        close(buffer->dma_buf_fd);
    *buffer = (struct video_buffer){.dma_buf_fd = -1, .descriptor_set = set};
}

/* Session restart (seek): keep only the frame on screen, free the other
 * imports so the new decoder can allocate its CAPTURE buffers, and keep
 * presenting the held frame until the first frame of the new stream. */
void hold_current_frame(struct app *app)
{
    need_vk("vkDeviceWaitIdle(hold)", vkDeviceWaitIdle(app->gpu.device));
    int keep = app->video.current;
    if (keep >= 0) {
        discard_held_frame(app);
        app->video.held = app->video.buffers[keep];
        app->video.held.producer_wait_pending = false;
        app->video.buffers[keep] = (struct video_buffer){
            .dma_buf_fd = -1, .descriptor_set = app->video.spare_set,
        };
        app->video.spare_set = VK_NULL_HANDLE;
        app->video.has_held = true;
    }
    for (uint32_t i = 0; i < app->video.buffer_count; ++i)
        if ((int)i != keep)
            destroy_video_buffer(app, &app->video.buffers[i]);
    app->video.current = -1;
    app->awaiting_frame = true;
    app->ui_dirty = true;
    printf("video_hold kept_frame=%d\n", keep);
}

void discard_held_frame(struct app *app)
{
    if (!app->video.has_held)
        return;
    need_vk("vkDeviceWaitIdle(discard held)", vkDeviceWaitIdle(app->gpu.device));
    VkDescriptorSet set = app->video.held.descriptor_set;
    destroy_video_buffer(app, &app->video.held);
    app->video.spare_set = set;
    app->video.has_held = false;
}

/* Mapping and importing a host buffer takes about 30 ms (8K P010, 100 MB),
 * so a worker imports them and the frame loop keeps the display rate. The
 * session waits for READY and sends nothing meanwhile. */
static void *video_import_worker(void *data)
{
    struct app *app = data;
    for (uint32_t i = 0; i < app->video.buffer_count; ++i)
        import_host_buffer(app, i);
    atomic_store(&app->video.import_done, true);
    return NULL;
}

static void finish_video_init(struct app *app)
{
    for (uint32_t i = 0; i < app->video.buffer_count; ++i)
        write_video_descriptor(app, app->video.buffers[i].descriptor_set,
                               &app->video.buffers[i]);
    app->video.current = -1;
    app->link.eos_received = false;
    send_ipc(app, NV12_IPC_READY, 0);
    send_volume(app);
    printf("video_reinit buffers=%u\n", app->video.buffer_count);
}

void reinitialize_video(struct app *app,
                        const struct nv12_ipc_message *message,
                        const struct msghdr *received)
{
    apply_init(app, message, received);
    if (app->video.buffer_kind == NV12_BUFFER_HOST) {
        atomic_store(&app->video.import_done, false);
        need(pthread_create(&app->video.import_thread, NULL,
                            video_import_worker, app) == 0,
             "start video import thread");
        app->video.importing = true;
        return;
    }
    for (uint32_t i = 0; i < app->video.buffer_count; ++i)
        import_video_buffer(app, i);
    finish_video_init(app);
}

/* True while the worker still imports; completes the INIT once it is done
 * (or, with wait, after waiting for it). */
bool video_import_pending(struct app *app, bool wait)
{
    if (!app->video.importing)
        return false;
    if (!wait && !atomic_load(&app->video.import_done))
        return true;
    pthread_join(app->video.import_thread, NULL);
    app->video.importing = false;
    if (!wait)
        finish_video_init(app);
    return false;
}
