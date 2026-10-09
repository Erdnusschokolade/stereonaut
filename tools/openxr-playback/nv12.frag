#version 450

/* The VkSampler bound to this descriptor owns a VkSamplerYcbcrConversion.
 * Sampling therefore performs the NV12 → RGB conversion in Vulkan. */
layout(set = 0, binding = 1) uniform sampler2D video_frame;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 output_color;

void main()
{
    output_color = vec4(texture(video_frame, uv).rgb, 1.0);
}
