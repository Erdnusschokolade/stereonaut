#ifndef STEREONAUT_GLTF_MODEL_H
#define STEREONAUT_GLTF_MODEL_H

#include <stddef.h>
#include <stdint.h>

/* A glTF 2.0 binary (GLB) as the runtime's controller models use it:
 * nodes with optional transform and mesh, triangle primitives with float
 * positions, normals and texture coordinates, a base colour texture (PNG)
 * and factor per material. Enough for XR_EXT_render_model assets
 * without glTF extensions; anything else fails to load. */

struct gltf_vertex {
    float position[3];
    float normal[3];
    float uv[2];
};

struct gltf_primitive {
    uint32_t first_index, index_count;
    int texture;             /* index into textures or -1 */
    float color[4];          /* base colour factor */
};

struct gltf_node {
    char name[64];
    int parent;              /* node index or -1 */
    float local[16];         /* column-major transform to the parent */
    uint32_t first_primitive, primitive_count;
};

struct gltf_texture {
    uint32_t width, height;
    uint8_t *rgba;
};

struct gltf_model {
    struct gltf_vertex *vertices;   /* per primitive, in node coordinates */
    uint32_t vertex_count;
    uint32_t *indices;              /* absolute vertex indices */
    uint32_t index_count;
    struct gltf_primitive *primitives;
    uint32_t primitive_count;
    struct gltf_node *nodes;        /* parents come before their children */
    uint32_t node_count;
    struct gltf_texture *textures;  /* identical images are shared */
    uint32_t texture_count;
};

int gltf_model_load(const uint8_t *glb, size_t size, struct gltf_model *model,
                    char *error, size_t error_size);
void gltf_model_free(struct gltf_model *model);

#endif
