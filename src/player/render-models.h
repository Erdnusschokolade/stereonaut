#ifndef STEREONAUT_RENDER_MODELS_H
#define STEREONAUT_RENDER_MODELS_H

#include <stdbool.h>
#include <stdint.h>

#include <openxr/openxr.h>

/* Controller models from the runtime (XR_EXT_render_model with
 * XR_EXT_interaction_render_model): for every device of the current
 * interaction profiles a glTF 2.0 binary (GLB), the space it is placed in
 * and the poses of its animatable nodes (buttons, sticks); `hands` are
 * the paths /user/hand/left and /user/hand/right. The Frame's
 * OpenXR headers (1.1.40) predate the extensions, so the declarations used
 * here are repeated in render-models.c when missing. */

#define RENDER_MODELS_MAX 4
#define RENDER_MODEL_NODE_NAME_SIZE 64

struct render_model {
    uint64_t handle;          /* XrRenderModelEXT */
    XrSpace space;            /* the model's origin */
    XrPath user_path;         /* /user/hand/left or right, else XR_NULL_PATH */
    uint8_t *glb;             /* asset data */
    uint32_t glb_size;
    uint32_t node_count;      /* animatable nodes */
    char (*node_names)[RENDER_MODEL_NODE_NAME_SIZE];
    XrPosef *node_poses;      /* per animatable node, from the last update */
    bool *node_visible;
};

struct render_models {
    bool supported;           /* both extensions enabled */
    uint32_t count;
    struct render_model models[RENDER_MODELS_MAX];
};

/* Adds the extensions (and XR_EXT_uuid, which they need with OpenXR 1.0)
 * to `names` if the runtime offers them; remembers whether it does. */
void render_models_request_extensions(struct render_models *models,
                                      const XrExtensionProperties *available,
                                      uint32_t available_count,
                                      const char **names,
                                      uint32_t *name_count);
/* (Re)loads the models of the current interaction profiles; call after
 * the action sets are attached and on
 * XR_TYPE_EVENT_DATA_INTERACTION_RENDER_MODELS_CHANGED_EXT. Each asset is
 * also written to $XDG_CACHE_HOME/stereonaut/render-model-N.glb for
 * inspection. */
void render_models_load(struct render_models *models, XrInstance instance,
                        XrSession session, const XrPath hands[2]);
/* Node poses for `time`. */
void render_models_update(struct render_models *models, XrTime time);
void render_models_free(struct render_models *models);
/* XrStructureType of the models-changed event. */
#define RENDER_MODELS_CHANGED_EVENT ((XrStructureType)1000301002)

#endif
