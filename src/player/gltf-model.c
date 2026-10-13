#define _POSIX_C_SOURCE 200809L
#include "gltf-model.h"

#include <json-c/json.h>
#include <png.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLB_MAGIC 0x46546C67U /* "glTF" */
#define CHUNK_JSON 0x4E4F534AU
#define CHUNK_BIN 0x004E4942U

#define COMPONENT_UNSIGNED_BYTE 5121
#define COMPONENT_UNSIGNED_SHORT 5123
#define COMPONENT_UNSIGNED_INT 5125
#define COMPONENT_FLOAT 5126
#define MODE_TRIANGLES 4

struct loader {
    json_object *root;
    const uint8_t *bin;
    size_t bin_size;
    struct gltf_model *model;
    char *error;
    size_t error_size;
    int *image_texture; /* texture index per glTF image, -1 not loaded */
    size_t image_count;
};

static int fail(struct loader *loader, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(loader->error, loader->error_size, format, arguments);
    va_end(arguments);
    return -1;
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static json_object *member(json_object *object, const char *key)
{
    json_object *value = NULL;
    return object && json_object_object_get_ex(object, key, &value) ? value
                                                                      : NULL;
}

static json_object *element(json_object *root, const char *array, int index)
{
    json_object *list = member(root, array);
    if (!list || !json_object_is_type(list, json_type_array) || index < 0 ||
        (size_t)index >= json_object_array_length(list))
        return NULL;
    return json_object_array_get_idx(list, (size_t)index);
}

static int integer(json_object *object, const char *key, int fallback)
{
    json_object *value = member(object, key);
    return value ? json_object_get_int(value) : fallback;
}

/* Bytes of a buffer view (only the GLB's own buffer 0). */
static const uint8_t *view_data(struct loader *loader, int index,
                                size_t *size, size_t *stride)
{
    json_object *view = element(loader->root, "bufferViews", index);
    if (!view || integer(view, "buffer", 0) != 0)
        return NULL;
    size_t offset = (size_t)integer(view, "byteOffset", 0);
    size_t length = (size_t)integer(view, "byteLength", 0);
    if (offset > loader->bin_size || length > loader->bin_size - offset)
        return NULL;
    *size = length;
    if (stride)
        *stride = (size_t)integer(view, "byteStride", 0);
    return loader->bin + offset;
}

/* An accessor's elements: `components` floats (float data) or indices. */
struct accessor {
    const uint8_t *data;
    size_t count, stride;
    int component_type;
};

static bool accessor_at(struct loader *loader, int index, int components,
                        struct accessor *out)
{
    json_object *accessor = element(loader->root, "accessors", index);
    if (!accessor)
        return false;
    static const struct {
        const char *name;
        int components;
    } types[] = {{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}};
    json_object *type = member(accessor, "type");
    int found = 0;
    for (size_t i = 0; type && i < sizeof(types) / sizeof(types[0]); ++i)
        if (strcmp(json_object_get_string(type), types[i].name) == 0)
            found = types[i].components;
    if (found != components)
        return false;
    int component_type = integer(accessor, "componentType", 0);
    size_t component_size =
        component_type == COMPONENT_UNSIGNED_BYTE    ? 1
        : component_type == COMPONENT_UNSIGNED_SHORT ? 2
        : component_type == COMPONENT_UNSIGNED_INT ||
                component_type == COMPONENT_FLOAT
            ? 4
            : 0;
    if (!component_size)
        return false;
    size_t view_size, stride;
    const uint8_t *data = view_data(loader, integer(accessor, "bufferView",
                                                    -1),
                                    &view_size, &stride);
    size_t offset = (size_t)integer(accessor, "byteOffset", 0);
    size_t count = (size_t)integer(accessor, "count", 0);
    size_t element_size = component_size * (size_t)components;
    if (!stride)
        stride = element_size;
    if (!data || !count || offset > view_size ||
        (count - 1) * stride + element_size > view_size - offset)
        return false;
    *out = (struct accessor){data + offset, count, stride, component_type};
    return true;
}

static float read_float(const struct accessor *accessor, size_t index,
                        int component)
{
    float value;
    memcpy(&value, accessor->data + index * accessor->stride +
                       (size_t)component * 4U,
           sizeof(value));
    return value;
}

static uint32_t read_index(const struct accessor *accessor, size_t index)
{
    const uint8_t *data = accessor->data + index * accessor->stride;
    if (accessor->component_type == COMPONENT_UNSIGNED_BYTE)
        return data[0];
    if (accessor->component_type == COMPONENT_UNSIGNED_SHORT)
        return (uint32_t)data[0] | (uint32_t)data[1] << 8;
    return read_u32(data);
}

/* Decodes a PNG image into RGBA (libpng's simplified API). Identical
 * images are decoded once. */
static int load_image(struct loader *loader, int image_index)
{
    if (image_index < 0 || (size_t)image_index >= loader->image_count)
        return -1;
    if (loader->image_texture[image_index] != -2)
        return loader->image_texture[image_index];
    loader->image_texture[image_index] = -1;
    json_object *image = element(loader->root, "images", image_index);
    size_t size;
    const uint8_t *data = view_data(loader, integer(image, "bufferView", -1),
                                    &size, NULL);
    if (!data)
        return -1;
    /* The runtime repeats one texture for every part: share it. */
    for (int other = 0; other < image_index; ++other) {
        json_object *earlier = element(loader->root, "images", other);
        size_t earlier_size;
        const uint8_t *earlier_data = view_data(
            loader, integer(earlier, "bufferView", -1), &earlier_size, NULL);
        if (earlier_data && earlier_size == size &&
            loader->image_texture[other] >= 0 &&
            memcmp(earlier_data, data, size) == 0)
            return loader->image_texture[image_index] =
                       loader->image_texture[other];
    }
    png_image png = {.version = PNG_IMAGE_VERSION};
    if (!png_image_begin_read_from_memory(&png, data, size))
        return -1;
    png.format = PNG_FORMAT_RGBA;
    uint8_t *rgba = malloc(PNG_IMAGE_SIZE(png));
    if (!rgba || !png_image_finish_read(&png, NULL, rgba, 0, NULL)) {
        free(rgba);
        png_image_free(&png);
        return -1;
    }
    struct gltf_model *model = loader->model;
    struct gltf_texture *textures = realloc(
        model->textures, (model->texture_count + 1) * sizeof(*textures));
    if (!textures) {
        free(rgba);
        return -1;
    }
    model->textures = textures;
    textures[model->texture_count] =
        (struct gltf_texture){png.width, png.height, rgba};
    return loader->image_texture[image_index] = (int)model->texture_count++;
}

static void material(struct loader *loader, int index,
                     struct gltf_primitive *primitive)
{
    primitive->texture = -1;
    for (int c = 0; c < 4; ++c)
        primitive->color[c] = 1.0f;
    json_object *pbr = member(element(loader->root, "materials", index),
                              "pbrMetallicRoughness");
    json_object *factor = member(pbr, "baseColorFactor");
    for (int c = 0; factor && c < 4 &&
                    (size_t)c < json_object_array_length(factor);
         ++c)
        primitive->color[c] =
            (float)json_object_get_double(json_object_array_get_idx(factor, c));
    json_object *texture = element(
        loader->root, "textures",
        integer(member(pbr, "baseColorTexture"), "index", -1));
    if (texture)
        primitive->texture = load_image(loader, integer(texture, "source", -1));
}

static int add_mesh(struct loader *loader, int mesh_index,
                    struct gltf_node *node)
{
    json_object *mesh = element(loader->root, "meshes", mesh_index);
    json_object *primitives = member(mesh, "primitives");
    if (!primitives)
        return fail(loader, "mesh %d has no primitives", mesh_index);
    struct gltf_model *model = loader->model;
    node->first_primitive = model->primitive_count;
    for (size_t p = 0; p < json_object_array_length(primitives); ++p) {
        json_object *primitive = json_object_array_get_idx(primitives, p);
        if (integer(primitive, "mode", MODE_TRIANGLES) != MODE_TRIANGLES)
            continue;
        json_object *attributes = member(primitive, "attributes");
        struct accessor position, normal = {0}, uv = {0}, indices = {0};
        if (!accessor_at(loader, integer(attributes, "POSITION", -1), 3,
                         &position) ||
            position.component_type != COMPONENT_FLOAT)
            return fail(loader, "mesh %d: no float positions", mesh_index);
        bool has_normal =
            accessor_at(loader, integer(attributes, "NORMAL", -1), 3,
                        &normal) &&
            normal.component_type == COMPONENT_FLOAT &&
            normal.count == position.count;
        bool has_uv = accessor_at(loader, integer(attributes, "TEXCOORD_0",
                                                  -1),
                                  2, &uv) &&
                      uv.component_type == COMPONENT_FLOAT &&
                      uv.count == position.count;
        bool indexed = member(primitive, "indices") != NULL;
        if (indexed &&
            (!accessor_at(loader, integer(primitive, "indices", -1), 1,
                          &indices) ||
             indices.component_type == COMPONENT_FLOAT))
            return fail(loader, "mesh %d: bad indices", mesh_index);
        size_t index_count = indexed ? indices.count : position.count;
        struct gltf_vertex *vertices = realloc(
            model->vertices,
            (model->vertex_count + position.count) * sizeof(*vertices));
        if (vertices)
            model->vertices = vertices;
        uint32_t *all_indices = realloc(
            model->indices, (model->index_count + index_count) *
                                sizeof(*all_indices));
        if (all_indices)
            model->indices = all_indices;
        struct gltf_primitive *list = realloc(
            model->primitives,
            (model->primitive_count + 1) * sizeof(*list));
        if (list)
            model->primitives = list;
        if (!vertices || !all_indices || !list)
            return fail(loader, "out of memory");
        uint32_t base = model->vertex_count;
        for (size_t v = 0; v < position.count; ++v) {
            struct gltf_vertex *vertex = &vertices[base + v];
            *vertex = (struct gltf_vertex){0};
            for (int c = 0; c < 3; ++c) {
                vertex->position[c] = read_float(&position, v, c);
                vertex->normal[c] = has_normal ? read_float(&normal, v, c)
                                               : (c == 2 ? 1.0f : 0.0f);
            }
            for (int c = 0; has_uv && c < 2; ++c)
                vertex->uv[c] = read_float(&uv, v, c);
        }
        for (size_t i = 0; i < index_count; ++i) {
            uint32_t value = indexed ? read_index(&indices, i) : (uint32_t)i;
            if (value >= position.count)
                return fail(loader, "mesh %d: index out of range",
                            mesh_index);
            all_indices[model->index_count + i] = base + value;
        }
        struct gltf_primitive *entry = &list[model->primitive_count];
        entry->first_index = model->index_count;
        entry->index_count = (uint32_t)index_count;
        material(loader, integer(primitive, "material", -1), entry);
        model->vertex_count += (uint32_t)position.count;
        model->index_count += (uint32_t)index_count;
        model->primitive_count++;
    }
    node->primitive_count = model->primitive_count - node->first_primitive;
    return 0;
}

/* Column-major matrix from a node's "matrix" or translation, rotation
 * (quaternion x, y, z, w) and scale. */
static void node_matrix(json_object *node, float m[16])
{
    json_object *matrix = member(node, "matrix");
    if (matrix && json_object_array_length(matrix) == 16) {
        for (int i = 0; i < 16; ++i)
            m[i] = (float)json_object_get_double(
                json_object_array_get_idx(matrix, i));
        return;
    }
    float t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
    json_object *value;
    if ((value = member(node, "translation")))
        for (int i = 0; i < 3 && (size_t)i < json_object_array_length(value);
             ++i)
            t[i] = (float)json_object_get_double(
                json_object_array_get_idx(value, i));
    if ((value = member(node, "rotation")))
        for (int i = 0; i < 4 && (size_t)i < json_object_array_length(value);
             ++i)
            q[i] = (float)json_object_get_double(
                json_object_array_get_idx(value, i));
    if ((value = member(node, "scale")))
        for (int i = 0; i < 3 && (size_t)i < json_object_array_length(value);
             ++i)
            s[i] = (float)json_object_get_double(
                json_object_array_get_idx(value, i));
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float r[9] = {
        1 - 2 * y * y - 2 * z * z, 2 * x * y + 2 * z * w,     2 * x * z - 2 * y * w,
        2 * x * y - 2 * z * w,     1 - 2 * x * x - 2 * z * z, 2 * y * z + 2 * x * w,
        2 * x * z + 2 * y * w,     2 * y * z - 2 * x * w,     1 - 2 * x * x - 2 * y * y,
    };
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row)
            m[column * 4 + row] = r[column * 3 + row] * s[column];
        m[column * 4 + 3] = 0.0f;
    }
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    m[15] = 1.0f;
}

static int add_node(struct loader *loader, int index, int parent, int depth)
{
    json_object *node = element(loader->root, "nodes", index);
    if (!node || depth > 32)
        return fail(loader, "bad node %d", index);
    struct gltf_model *model = loader->model;
    struct gltf_node *nodes = realloc(model->nodes, (model->node_count + 1) *
                                                        sizeof(*nodes));
    if (!nodes)
        return fail(loader, "out of memory");
    model->nodes = nodes;
    int self = (int)model->node_count++;
    struct gltf_node *entry = &nodes[self];
    *entry = (struct gltf_node){.parent = parent};
    json_object *name = member(node, "name");
    if (name)
        snprintf(entry->name, sizeof(entry->name), "%s",
                 json_object_get_string(name));
    node_matrix(node, entry->local);
    if (member(node, "mesh")) {
        struct gltf_node copy = *entry;
        if (add_mesh(loader, integer(node, "mesh", -1), &copy) < 0)
            return -1;
        model->nodes[self] = copy;
    }
    json_object *children = member(node, "children");
    for (size_t i = 0; children && i < json_object_array_length(children);
         ++i)
        if (add_node(loader, json_object_get_int(
                                 json_object_array_get_idx(children, i)),
                     self, depth + 1) < 0)
            return -1;
    return 0;
}

int gltf_model_load(const uint8_t *glb, size_t size, struct gltf_model *model,
                    char *error, size_t error_size)
{
    *model = (struct gltf_model){0};
    struct loader loader = {.model = model, .error = error,
                            .error_size = error_size};
    if (size < 20 || read_u32(glb) != GLB_MAGIC || read_u32(glb + 4) != 2 ||
        read_u32(glb + 8) > size)
        return fail(&loader, "not a glTF 2.0 binary");
    size_t json_size = read_u32(glb + 12);
    if (read_u32(glb + 16) != CHUNK_JSON || json_size > size - 20)
        return fail(&loader, "no JSON chunk");
    size_t bin_header = 20 + json_size;
    if (bin_header + 8 <= size && read_u32(glb + bin_header + 4) == CHUNK_BIN) {
        size_t bin_size = read_u32(glb + bin_header);
        if (bin_size <= size - bin_header - 8) {
            loader.bin = glb + bin_header + 8;
            loader.bin_size = bin_size;
        }
    }
    json_tokener *tokener = json_tokener_new();
    if (!tokener)
        return fail(&loader, "out of memory");
    loader.root = json_tokener_parse_ex(tokener, (const char *)glb + 20,
                                        (int)json_size);
    json_tokener_free(tokener);
    if (!loader.root)
        return fail(&loader, "bad JSON chunk");
    if (member(loader.root, "extensionsRequired")) {
        json_object_put(loader.root);
        return fail(&loader, "requires glTF extensions");
    }
    json_object *images = member(loader.root, "images");
    loader.image_count = images ? json_object_array_length(images) : 0;
    loader.image_texture = malloc((loader.image_count + 1) * sizeof(int));
    for (size_t i = 0; loader.image_texture && i < loader.image_count; ++i)
        loader.image_texture[i] = -2;
    json_object *scene = element(loader.root, "scenes",
                                 integer(loader.root, "scene", 0));
    json_object *roots = member(scene, "nodes");
    int result = loader.image_texture && roots ? 0
                 : fail(&loader, "no scene");
    for (size_t i = 0; result == 0 && i < json_object_array_length(roots);
         ++i)
        result = add_node(&loader,
                          json_object_get_int(json_object_array_get_idx(roots,
                                                                        i)),
                          -1, 0);
    free(loader.image_texture);
    json_object_put(loader.root);
    if (result < 0)
        gltf_model_free(model);
    return result;
}

void gltf_model_free(struct gltf_model *model)
{
    for (uint32_t i = 0; i < model->texture_count; ++i)
        free(model->textures[i].rgba);
    free(model->textures);
    free(model->vertices);
    free(model->indices);
    free(model->primitives);
    free(model->nodes);
    *model = (struct gltf_model){0};
}
