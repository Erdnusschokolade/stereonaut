/* Probe which controller input paths the OpenXR runtime accepts as suggested
 * bindings. Creates an instance and action set only; no session, no rendering
 * and no persistent runtime configuration. */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openxr/openxr.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static XrInstance instance;

static XrPath path(const char *text)
{
    XrPath result = XR_NULL_PATH;
    XrResult status = xrStringToPath(instance, text, &result);
    if (XR_FAILED(status)) {
        fprintf(stderr, "xrStringToPath(%s): %d\n", text, status);
        exit(2);
    }
    return result;
}

static bool has_extension(const char *name, XrExtensionProperties *list,
                          uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        if (strcmp(list[i].extensionName, name) == 0)
            return true;
    return false;
}

static void try_binding(XrAction action, const char *profile,
                        const char *binding)
{
    XrActionSuggestedBinding suggested = {action, path(binding)};
    XrInteractionProfileSuggestedBinding suggestion = {
        .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .interactionProfile = path(profile),
        .countSuggestedBindings = 1,
        .suggestedBindings = &suggested,
    };
    XrResult result = xrSuggestInteractionProfileBindings(instance, &suggestion);
    printf("%-52s %-52s %s (%d)\n", profile, binding,
           XR_SUCCEEDED(result) ? "ACCEPTED" : "rejected", result);
}

int main(void)
{
    uint32_t count = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &count, NULL);
    XrExtensionProperties *list = calloc(count, sizeof(*list));
    for (uint32_t i = 0; i < count; ++i)
        list[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, count, &count, list);

    const char *wanted[] = {
        "XR_VALVE_frame_controller_interaction",
        "XR_KHR_generic_controller",
    };
    const char *enabled[ARRAY_COUNT(wanted)];
    uint32_t enabled_count = 0;
    for (size_t i = 0; i < ARRAY_COUNT(wanted); ++i) {
        bool present = has_extension(wanted[i], list, count);
        printf("extension %-44s %s\n", wanted[i],
               present ? "available" : "missing");
        if (present)
            enabled[enabled_count++] = wanted[i];
    }

    XrInstanceCreateInfo info = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {.applicationVersion = 1, .engineVersion = 1,
                            .apiVersion = XR_MAKE_VERSION(1, 0, 0)},
        .enabledExtensionCount = enabled_count,
        .enabledExtensionNames = enabled,
    };
    snprintf(info.applicationInfo.applicationName,
             sizeof(info.applicationInfo.applicationName),
             "stereonaut-input-profile-probe");
    XrResult result = xrCreateInstance(&info, &instance);
    if (XR_FAILED(result)) {
        fprintf(stderr, "xrCreateInstance: %d\n", result);
        return 2;
    }

    XrActionSetCreateInfo set_info = {.type = XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(set_info.actionSetName, sizeof(set_info.actionSetName), "probe");
    snprintf(set_info.localizedActionSetName,
             sizeof(set_info.localizedActionSetName), "Probe");
    XrActionSet set;
    if (XR_FAILED(xrCreateActionSet(instance, &set_info, &set))) return 2;

    XrAction boolean_action, pose_action, vector_action;
    XrActionCreateInfo action_info = {
        .type = XR_TYPE_ACTION_CREATE_INFO,
        .actionType = XR_ACTION_TYPE_BOOLEAN_INPUT,
    };
    snprintf(action_info.actionName, sizeof(action_info.actionName), "bool");
    snprintf(action_info.localizedActionName,
             sizeof(action_info.localizedActionName), "Bool");
    if (XR_FAILED(xrCreateAction(set, &action_info, &boolean_action)))
        return 2;
    action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
    snprintf(action_info.actionName, sizeof(action_info.actionName), "pose");
    snprintf(action_info.localizedActionName,
             sizeof(action_info.localizedActionName), "Pose");
    if (XR_FAILED(xrCreateAction(set, &action_info, &pose_action)))
        return 2;
    action_info.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    snprintf(action_info.actionName, sizeof(action_info.actionName), "stick");
    snprintf(action_info.localizedActionName,
             sizeof(action_info.localizedActionName), "Stick");
    if (XR_FAILED(xrCreateAction(set, &action_info, &vector_action)))
        return 2;

    const char *profiles[] = {
        "/interaction_profiles/valve/frame_controller",
        "/interaction_profiles/valve/frame_controller_valve",
        "/interaction_profiles/khr/generic_controller",
        "/interaction_profiles/valve/index_controller",
        "/interaction_profiles/khr/simple_controller",
    };
    const char *booleans[] = {
        "/user/hand/right/input/squeeze/click",
        "/user/hand/right/input/squeeze/value",
        "/user/hand/right/input/grip/click",
        "/user/hand/right/input/grip/value",
        "/user/hand/right/input/trigger/click",
        "/user/hand/right/input/trigger/value",
        "/user/hand/right/input/select/click",
        "/user/hand/right/input/menu/click",
    };
    for (size_t p = 0; p < ARRAY_COUNT(profiles); ++p) {
        try_binding(pose_action, profiles[p],
                    "/user/hand/right/input/aim/pose");
        try_binding(pose_action, profiles[p],
                    "/user/hand/right/input/grip/pose");
        for (size_t b = 0; b < ARRAY_COUNT(booleans); ++b)
            try_binding(boolean_action, profiles[p], booleans[b]);
        try_binding(vector_action, profiles[p],
                    "/user/hand/right/input/thumbstick");
        try_binding(vector_action, profiles[p],
                    "/user/hand/right/input/trackpad");
    }
    xrDestroyInstance(instance);
    free(list);
    return 0;
}
