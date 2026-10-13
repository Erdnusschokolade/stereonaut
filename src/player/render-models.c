#define _POSIX_C_SOURCE 200809L
#include "render-models.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef XR_EXT_render_model
/* From the OpenXR 1.1.60 headers (XR_EXT_render_model 1 and
 * XR_EXT_interaction_render_model 1). */
#define XR_EXT_RENDER_MODEL_EXTENSION_NAME "XR_EXT_render_model"
#define XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME \
    "XR_EXT_interaction_render_model"
#define XR_MAX_RENDER_MODEL_ASSET_NODE_NAME_SIZE_EXT 64
XR_DEFINE_ATOM(XrRenderModelIdEXT)
XR_DEFINE_HANDLE(XrRenderModelEXT)
XR_DEFINE_HANDLE(XrRenderModelAssetEXT)
#define XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT ((XrStructureType)1000300000)
#define XR_TYPE_RENDER_MODEL_PROPERTIES_GET_INFO_EXT \
    ((XrStructureType)1000300001)
#define XR_TYPE_RENDER_MODEL_PROPERTIES_EXT ((XrStructureType)1000300002)
#define XR_TYPE_RENDER_MODEL_SPACE_CREATE_INFO_EXT ((XrStructureType)1000300003)
#define XR_TYPE_RENDER_MODEL_STATE_GET_INFO_EXT ((XrStructureType)1000300004)
#define XR_TYPE_RENDER_MODEL_STATE_EXT ((XrStructureType)1000300005)
#define XR_TYPE_RENDER_MODEL_ASSET_CREATE_INFO_EXT ((XrStructureType)1000300006)
#define XR_TYPE_RENDER_MODEL_ASSET_DATA_GET_INFO_EXT \
    ((XrStructureType)1000300007)
#define XR_TYPE_RENDER_MODEL_ASSET_DATA_EXT ((XrStructureType)1000300008)
#define XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_GET_INFO_EXT \
    ((XrStructureType)1000300009)
#define XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_EXT ((XrStructureType)1000300010)
#define XR_TYPE_INTERACTION_RENDER_MODEL_IDS_ENUMERATE_INFO_EXT \
    ((XrStructureType)1000301000)
#define XR_TYPE_INTERACTION_RENDER_MODEL_TOP_LEVEL_USER_PATH_GET_INFO_EXT \
    ((XrStructureType)1000301003)
typedef struct XrRenderModelCreateInfoEXT {
    XrStructureType type;
    const void *XR_MAY_ALIAS next;
    XrRenderModelIdEXT renderModelId;
    uint32_t gltfExtensionCount;
    const char *const *gltfExtensions;
} XrRenderModelCreateInfoEXT;
typedef struct XrRenderModelPropertiesGetInfoEXT {
    XrStructureType type;
    const void *XR_MAY_ALIAS next;
} XrRenderModelPropertiesGetInfoEXT;
typedef struct XrRenderModelPropertiesEXT {
    XrStructureType type;
    void *XR_MAY_ALIAS next;
    XrUuidEXT cacheId;
    uint32_t animatableNodeCount;
} XrRenderModelPropertiesEXT;
typedef struct XrRenderModelSpaceCreateInfoEXT {
    XrStructureType type;
    const void *XR_MAY_ALIAS next;
    XrRenderModelEXT renderModel;
} XrRenderModelSpaceCreateInfoEXT;
typedef struct XrRenderModelStateGetInfoEXT {
    XrStructureType type;
    const void *XR_MAY_ALIAS next;
    XrTime displayTime;
} XrRenderModelStateGetInfoEXT;
typedef struct XrRenderModelNodeStateEXT {
    XrPosef nodePose;
    XrBool32 isVisible;
} XrRenderModelNodeStateEXT;
typedef struct XrRenderModelStateEXT {
    XrStructureType type;
    void *XR_MAY_ALIAS next;
    uint32_t nodeStateCount;
    XrRenderModelNodeStateEXT *nodeStates;
} XrRenderModelStateEXT;
typedef struct XrRenderModelAssetCreateInfoEXT {
    XrStructureType type;
    const void *XR_MAY_ALIAS next;
    XrUuidEXT cacheId;
} XrRenderModelAssetCreateInfoEXT;
typedef struct XrRenderModelAssetDataGetInfoEXT {
    XrStructureType type;
    const void *XR_MAY_ALIAS next;
} XrRenderModelAssetDataGetInfoEXT;
typedef struct XrRenderModelAssetDataEXT {
    XrStructureType type;
    void *XR_MAY_ALIAS next;
    uint32_t bufferCapacityInput;
    uint32_t bufferCountOutput;
    uint8_t *buffer;
} XrRenderModelAssetDataEXT;
typedef struct XrRenderModelAssetPropertiesGetInfoEXT {
    XrStructureType type;
    const void *XR_MAY_ALIAS next;
} XrRenderModelAssetPropertiesGetInfoEXT;
typedef struct XrRenderModelAssetNodePropertiesEXT {
    char uniqueName[XR_MAX_RENDER_MODEL_ASSET_NODE_NAME_SIZE_EXT];
} XrRenderModelAssetNodePropertiesEXT;
typedef struct XrRenderModelAssetPropertiesEXT {
    XrStructureType type;
    void *XR_MAY_ALIAS next;
    uint32_t nodePropertyCount;
    XrRenderModelAssetNodePropertiesEXT *nodeProperties;
} XrRenderModelAssetPropertiesEXT;
typedef struct XrInteractionRenderModelIdsEnumerateInfoEXT {
    XrStructureType type;
    const void *XR_MAY_ALIAS next;
} XrInteractionRenderModelIdsEnumerateInfoEXT;
typedef struct XrInteractionRenderModelTopLevelUserPathGetInfoEXT {
    XrStructureType type;
    const void *XR_MAY_ALIAS next;
    uint32_t topLevelUserPathCount;
    const XrPath *topLevelUserPaths;
} XrInteractionRenderModelTopLevelUserPathGetInfoEXT;
typedef XrResult(XRAPI_PTR *PFN_xrCreateRenderModelEXT)(
    XrSession, const XrRenderModelCreateInfoEXT *, XrRenderModelEXT *);
typedef XrResult(XRAPI_PTR *PFN_xrDestroyRenderModelEXT)(XrRenderModelEXT);
typedef XrResult(XRAPI_PTR *PFN_xrGetRenderModelPropertiesEXT)(
    XrRenderModelEXT, const XrRenderModelPropertiesGetInfoEXT *,
    XrRenderModelPropertiesEXT *);
typedef XrResult(XRAPI_PTR *PFN_xrCreateRenderModelSpaceEXT)(
    XrSession, const XrRenderModelSpaceCreateInfoEXT *, XrSpace *);
typedef XrResult(XRAPI_PTR *PFN_xrCreateRenderModelAssetEXT)(
    XrSession, const XrRenderModelAssetCreateInfoEXT *,
    XrRenderModelAssetEXT *);
typedef XrResult(XRAPI_PTR *PFN_xrDestroyRenderModelAssetEXT)(
    XrRenderModelAssetEXT);
typedef XrResult(XRAPI_PTR *PFN_xrGetRenderModelAssetDataEXT)(
    XrRenderModelAssetEXT, const XrRenderModelAssetDataGetInfoEXT *,
    XrRenderModelAssetDataEXT *);
typedef XrResult(XRAPI_PTR *PFN_xrGetRenderModelAssetPropertiesEXT)(
    XrRenderModelAssetEXT, const XrRenderModelAssetPropertiesGetInfoEXT *,
    XrRenderModelAssetPropertiesEXT *);
typedef XrResult(XRAPI_PTR *PFN_xrGetRenderModelStateEXT)(
    XrRenderModelEXT, const XrRenderModelStateGetInfoEXT *,
    XrRenderModelStateEXT *);
typedef XrResult(XRAPI_PTR *PFN_xrEnumerateInteractionRenderModelIdsEXT)(
    XrSession, const XrInteractionRenderModelIdsEnumerateInfoEXT *, uint32_t,
    uint32_t *, XrRenderModelIdEXT *);
typedef XrResult(XRAPI_PTR *PFN_xrGetRenderModelPoseTopLevelUserPathEXT)(
    XrRenderModelEXT, const XrInteractionRenderModelTopLevelUserPathGetInfoEXT *,
    XrPath *);
#endif

static struct {
    PFN_xrCreateRenderModelEXT create;
    PFN_xrDestroyRenderModelEXT destroy;
    PFN_xrGetRenderModelPropertiesEXT properties;
    PFN_xrCreateRenderModelSpaceEXT create_space;
    PFN_xrCreateRenderModelAssetEXT create_asset;
    PFN_xrDestroyRenderModelAssetEXT destroy_asset;
    PFN_xrGetRenderModelAssetDataEXT asset_data;
    PFN_xrGetRenderModelAssetPropertiesEXT asset_properties;
    PFN_xrGetRenderModelStateEXT state;
    PFN_xrEnumerateInteractionRenderModelIdsEXT enumerate;
    PFN_xrGetRenderModelPoseTopLevelUserPathEXT user_path;
} api;

void render_models_request_extensions(struct render_models *models,
                                      const XrExtensionProperties *available,
                                      uint32_t available_count,
                                      const char **names,
                                      uint32_t *name_count)
{
    bool render_model = false, interaction = false, uuid = false;
    for (uint32_t i = 0; i < available_count; ++i) {
        const char *name = available[i].extensionName;
        render_model |= strcmp(name, XR_EXT_RENDER_MODEL_EXTENSION_NAME) == 0;
        interaction |= strcmp(name,
                              XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME) ==
                       0;
        uuid |= strcmp(name, "XR_EXT_uuid") == 0;
    }
    models->supported = render_model && interaction && uuid;
    if (!models->supported)
        return;
    names[(*name_count)++] = XR_EXT_RENDER_MODEL_EXTENSION_NAME;
    names[(*name_count)++] = XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME;
    names[(*name_count)++] = "XR_EXT_uuid";
}

static bool load_functions(XrInstance instance)
{
    struct {
        const char *name;
        PFN_xrVoidFunction *function;
    } table[] = {
        {"xrCreateRenderModelEXT", (PFN_xrVoidFunction *)&api.create},
        {"xrDestroyRenderModelEXT", (PFN_xrVoidFunction *)&api.destroy},
        {"xrGetRenderModelPropertiesEXT",
         (PFN_xrVoidFunction *)&api.properties},
        {"xrCreateRenderModelSpaceEXT",
         (PFN_xrVoidFunction *)&api.create_space},
        {"xrCreateRenderModelAssetEXT",
         (PFN_xrVoidFunction *)&api.create_asset},
        {"xrDestroyRenderModelAssetEXT",
         (PFN_xrVoidFunction *)&api.destroy_asset},
        {"xrGetRenderModelAssetDataEXT",
         (PFN_xrVoidFunction *)&api.asset_data},
        {"xrGetRenderModelAssetPropertiesEXT",
         (PFN_xrVoidFunction *)&api.asset_properties},
        {"xrGetRenderModelStateEXT", (PFN_xrVoidFunction *)&api.state},
        {"xrEnumerateInteractionRenderModelIdsEXT",
         (PFN_xrVoidFunction *)&api.enumerate},
        {"xrGetRenderModelPoseTopLevelUserPathEXT",
         (PFN_xrVoidFunction *)&api.user_path},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
        if (XR_FAILED(xrGetInstanceProcAddr(instance, table[i].name,
                                            table[i].function)) ||
            !*table[i].function) {
            fprintf(stderr, "render_models missing %s\n", table[i].name);
            return false;
        }
    return true;
}

static void free_model(struct render_model *model)
{
    if (model->space != XR_NULL_HANDLE)
        xrDestroySpace(model->space);
    if (model->handle)
        api.destroy((XrRenderModelEXT)(uintptr_t)model->handle);
    free(model->glb);
    free(model->node_names);
    free(model->node_poses);
    free(model->node_visible);
    memset(model, 0, sizeof(*model));
}

void render_models_free(struct render_models *models)
{
    for (uint32_t i = 0; i < models->count; ++i)
        free_model(&models->models[i]);
    models->count = 0;
}

/* Writes the asset to the cache directory for inspection. */
static void keep_copy(const struct render_model *model, uint32_t index)
{
    const char *cache = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char directory[1024], path[1100];
    if (cache && cache[0])
        snprintf(directory, sizeof(directory), "%s/stereonaut", cache);
    else if (home && home[0])
        snprintf(directory, sizeof(directory), "%s/.cache/stereonaut", home);
    else
        return;
    mkdir(directory, 0755);
    snprintf(path, sizeof(path), "%s/render-model-%u.glb", directory, index);
    FILE *file = fopen(path, "wb");
    if (!file)
        return;
    fwrite(model->glb, 1, model->glb_size, file);
    fclose(file);
}

static bool load_model(struct render_model *model, XrSession session,
                       XrRenderModelIdEXT id, uint32_t index,
                       const XrPath hands[2])
{
    XrRenderModelCreateInfoEXT create = {
        .type = XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT,
        .renderModelId = id,
    };
    XrRenderModelEXT handle = XR_NULL_HANDLE;
    XrResult result = api.create(session, &create, &handle);
    if (XR_FAILED(result)) {
        fprintf(stderr, "render_model create id=%" PRIu64 " result=%d\n",
                (uint64_t)id, result);
        return false;
    }
    model->handle = (uint64_t)(uintptr_t)handle;
    XrRenderModelPropertiesGetInfoEXT get = {
        .type = XR_TYPE_RENDER_MODEL_PROPERTIES_GET_INFO_EXT,
    };
    XrRenderModelPropertiesEXT properties = {
        .type = XR_TYPE_RENDER_MODEL_PROPERTIES_EXT,
    };
    if (XR_FAILED(api.properties(handle, &get, &properties)))
        return false;
    XrRenderModelSpaceCreateInfoEXT space = {
        .type = XR_TYPE_RENDER_MODEL_SPACE_CREATE_INFO_EXT,
        .renderModel = handle,
    };
    if (XR_FAILED(api.create_space(session, &space, &model->space)))
        return false;
    XrRenderModelAssetCreateInfoEXT asset_create = {
        .type = XR_TYPE_RENDER_MODEL_ASSET_CREATE_INFO_EXT,
        .cacheId = properties.cacheId,
    };
    XrRenderModelAssetEXT asset = XR_NULL_HANDLE;
    if (XR_FAILED(api.create_asset(session, &asset_create, &asset)))
        return false;
    XrRenderModelAssetDataGetInfoEXT data_get = {
        .type = XR_TYPE_RENDER_MODEL_ASSET_DATA_GET_INFO_EXT,
    };
    XrRenderModelAssetDataEXT data = {
        .type = XR_TYPE_RENDER_MODEL_ASSET_DATA_EXT,
    };
    bool ok = XR_SUCCEEDED(api.asset_data(asset, &data_get, &data)) &&
              data.bufferCountOutput > 0;
    if (ok) {
        model->glb = malloc(data.bufferCountOutput);
        data.bufferCapacityInput = data.bufferCountOutput;
        data.buffer = model->glb;
        ok = model->glb &&
             XR_SUCCEEDED(api.asset_data(asset, &data_get, &data));
        model->glb_size = data.bufferCountOutput;
    }
    model->node_count = properties.animatableNodeCount;
    if (ok && model->node_count) {
        XrRenderModelAssetNodePropertiesEXT *nodes =
            calloc(model->node_count, sizeof(*nodes));
        model->node_names = calloc(model->node_count,
                                   sizeof(*model->node_names));
        model->node_poses = calloc(model->node_count,
                                   sizeof(*model->node_poses));
        model->node_visible = calloc(model->node_count,
                                     sizeof(*model->node_visible));
        XrRenderModelAssetPropertiesGetInfoEXT names_get = {
            .type = XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_GET_INFO_EXT,
        };
        XrRenderModelAssetPropertiesEXT names = {
            .type = XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_EXT,
            .nodePropertyCount = model->node_count,
            .nodeProperties = nodes,
        };
        ok = nodes && model->node_names && model->node_poses &&
             model->node_visible &&
             XR_SUCCEEDED(api.asset_properties(asset, &names_get, &names));
        for (uint32_t i = 0; ok && i < model->node_count; ++i) {
            memcpy(model->node_names[i], nodes[i].uniqueName,
                   RENDER_MODEL_NODE_NAME_SIZE);
            model->node_names[i][RENDER_MODEL_NODE_NAME_SIZE - 1] = '\0';
            model->node_poses[i].orientation.w = 1.0f;
        }
        free(nodes);
    }
    api.destroy_asset(asset);
    XrInteractionRenderModelTopLevelUserPathGetInfoEXT user = {
        .type = XR_TYPE_INTERACTION_RENDER_MODEL_TOP_LEVEL_USER_PATH_GET_INFO_EXT,
        .topLevelUserPathCount = 2,
        .topLevelUserPaths = hands,
    };
    if (XR_FAILED(api.user_path(handle, &user, &model->user_path)))
        model->user_path = XR_NULL_PATH;
    if (ok)
        keep_copy(model, index);
    printf("render_model index=%u id=%" PRIu64 " bytes=%u nodes=%u ok=%d\n",
           index, (uint64_t)id, model->glb_size, model->node_count, ok);
    for (uint32_t i = 0; ok && i < model->node_count; ++i)
        printf("render_model_node index=%u node=%u name=%s\n", index, i,
               model->node_names[i]);
    return ok;
}

void render_models_load(struct render_models *models, XrInstance instance,
                        XrSession session, const XrPath hands[2])
{
    render_models_free(models);
    if (!models->supported)
        return;
    static bool functions;
    if (!functions && !(functions = load_functions(instance))) {
        models->supported = false;
        return;
    }
    XrInteractionRenderModelIdsEnumerateInfoEXT info = {
        .type = XR_TYPE_INTERACTION_RENDER_MODEL_IDS_ENUMERATE_INFO_EXT,
    };
    XrRenderModelIdEXT ids[RENDER_MODELS_MAX];
    uint32_t count = 0;
    XrResult result = api.enumerate(session, &info, RENDER_MODELS_MAX,
                                    &count, ids);
    if (XR_FAILED(result)) {
        fprintf(stderr, "render_models enumerate result=%d\n", result);
        return;
    }
    printf("render_models ids=%u\n", count);
    for (uint32_t i = 0; i < count && i < RENDER_MODELS_MAX; ++i) {
        struct render_model *model = &models->models[models->count];
        if (load_model(model, session, ids[i], i, hands))
            models->count++;
        else
            free_model(model);
    }
}

void render_models_update(struct render_models *models, XrTime time)
{
    for (uint32_t i = 0; i < models->count; ++i) {
        struct render_model *model = &models->models[i];
        if (!model->node_count)
            continue;
        XrRenderModelNodeStateEXT states[64];
        uint32_t count = model->node_count < 64 ? model->node_count : 64;
        XrRenderModelStateGetInfoEXT get = {
            .type = XR_TYPE_RENDER_MODEL_STATE_GET_INFO_EXT,
            .displayTime = time,
        };
        XrRenderModelStateEXT state = {
            .type = XR_TYPE_RENDER_MODEL_STATE_EXT,
            .nodeStateCount = count,
            .nodeStates = states,
        };
        if (XR_FAILED(api.state(
                (XrRenderModelEXT)(uintptr_t)model->handle, &get, &state)))
            continue;
        for (uint32_t n = 0; n < count; ++n) {
            model->node_poses[n] = states[n].nodePose;
            model->node_visible[n] = states[n].isVisible;
        }
    }
}
