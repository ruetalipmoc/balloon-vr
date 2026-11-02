#include "xr_session.h"
#include "xr_glx.h"

#include <openxr/openxr.h>

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

/* raylib's GL loader is private to the library. The real libGL entry point
 * is enough to read the context version while that context is current. */
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#endif
#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION 0x821C
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
extern void glDeleteRenderbuffers(int n, const unsigned int* renderbuffers);
extern void glGetIntegerv(unsigned int pname, int* data);

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Layout copies of the KHR structs. Including openxr_platform.h here would
 * pull glx.h into the same unit as raylib's GL loader. */
typedef struct BalloonXrSwapchainImage {
    XrStructureType type;
    void* next;
    unsigned int image;
} BalloonXrSwapchainImage;

typedef struct BalloonXrGlRequirements {
    XrStructureType type;
    void* next;
    XrVersion minApiVersionSupported;
    XrVersion maxApiVersionSupported;
} BalloonXrGlRequirements;

typedef XrResult (*BalloonPfnGetGlRequirements)(XrInstance, XrSystemId, BalloonXrGlRequirements*);

#define MAX_VIEWS 2
#define MAX_SWAP_IMAGES 8

typedef struct EyeTarget {
    XrSwapchain swapchain;
    uint32_t width;
    uint32_t height;
    uint32_t imageCount;
    unsigned int images[MAX_SWAP_IMAGES];
    unsigned int fbo;
    unsigned int depth;
    int acquired;
    uint32_t imageIndex;
    float proj[16];
    float view[16];
    XrPosef pose;
    XrFovf fov;
} EyeTarget;

static XrInstance g_instance = 0;
static XrSystemId g_system = 0;
static XrSession g_session = 0;
static XrSpace g_space = 0;
static int g_stage = 0;
static int g_sessionRunning = 0;
static int g_wantsQuit = 0;
static int g_ready = 0;
static XrEnvironmentBlendMode g_blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

static XrActionSet g_actions = 0;
static XrAction g_gripValue = 0;
static XrAction g_gripClick = 0;
static XrAction g_pose = 0;
static XrAction g_haptic = 0;
static XrSpace g_handSpace[2] = { 0, 0 };
static XrPath g_handPath[2] = { 0, 0 };
static int g_gripWas[2] = { 0, 0 };

static EyeTarget g_eyes[MAX_VIEWS];
static int g_eyeCount = 0;
static XrFrameState g_frameState;
static int g_frameOpen = 0;

static float g_originX = 0.0f;
static float g_originZ = 0.0f;
static float g_floorLift = 0.0f;
static int g_needRecenter = 1;
static int g_floorChosen = 0;

static XrCompositionLayerProjectionView g_projViews[MAX_VIEWS];
static XrCompositionLayerProjection g_layer;

static void XrLogResult(const char* what, XrResult result)
{
    char buffer[XR_MAX_RESULT_STRING_SIZE];
    buffer[0] = 0;
    if (g_instance && XR_FAILED(result)) xrResultToString(g_instance, result, buffer);
    if (buffer[0]) fprintf(stderr, "[xr] %s failed: %s (%d)\n", what, buffer, (int)result);
    else fprintf(stderr, "[xr] %s failed: %d\n", what, (int)result);
}

static int XrOk(const char* what, XrResult result)
{
    if (XR_SUCCEEDED(result)) return 1;
    XrLogResult(what, result);
    return 0;
}

static void SuggestProfile(const char* profile, const char* gripSuffix, int gripIsClick)
{
    XrPath profilePath = 0;
    if (!XrOk(profile, xrStringToPath(g_instance, profile, &profilePath))) return;

    XrActionSuggestedBinding binds[8];
    int count = 0;
    const char* sides[2] = { "/user/hand/left", "/user/hand/right" };
    for (int i = 0; i < 2; i++) {
        char path[160];
        XrPath bound = 0;
        snprintf(path, sizeof(path), "%s/%s", sides[i], gripSuffix);
        if (XR_FAILED(xrStringToPath(g_instance, path, &bound))) continue;
        binds[count].action = gripIsClick ? g_gripClick : g_gripValue;
        binds[count].binding = bound;
        count++;

        snprintf(path, sizeof(path), "%s/input/grip/pose", sides[i]);
        if (XR_FAILED(xrStringToPath(g_instance, path, &bound))) continue;
        binds[count].action = g_pose;
        binds[count].binding = bound;
        count++;

        snprintf(path, sizeof(path), "%s/output/haptic", sides[i]);
        if (XR_FAILED(xrStringToPath(g_instance, path, &bound))) continue;
        binds[count].action = g_haptic;
        binds[count].binding = bound;
        count++;
    }

    XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggested.interactionProfile = profilePath;
    suggested.countSuggestedBindings = (uint32_t)count;
    suggested.suggestedBindings = binds;
    XrResult result = xrSuggestInteractionProfileBindings(g_instance, &suggested);
    if (XR_FAILED(result)) XrLogResult(profile, result);
    else fprintf(stderr, "[xr] bindings suggested for %s\n", profile);
}

static int CreateActions(void)
{
    XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy(setInfo.actionSetName, "gondola", sizeof(setInfo.actionSetName) - 1);
    strncpy(setInfo.localizedActionSetName, "Gondola", sizeof(setInfo.localizedActionSetName) - 1);
    setInfo.priority = 0;
    if (!XrOk("action set", xrCreateActionSet(g_instance, &setInfo, &g_actions))) return 0;

    xrStringToPath(g_instance, "/user/hand/left", &g_handPath[0]);
    xrStringToPath(g_instance, "/user/hand/right", &g_handPath[1]);

    XrActionCreateInfo action = { XR_TYPE_ACTION_CREATE_INFO };
    action.countSubactionPaths = 2;
    action.subactionPaths = g_handPath;

    action.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    strncpy(action.actionName, "grip_value", sizeof(action.actionName) - 1);
    strncpy(action.localizedActionName, "Grip", sizeof(action.localizedActionName) - 1);
    if (!XrOk("grip value", xrCreateAction(g_actions, &action, &g_gripValue))) return 0;

    action.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    strncpy(action.actionName, "grip_click", sizeof(action.actionName) - 1);
    strncpy(action.localizedActionName, "Grip Click", sizeof(action.localizedActionName) - 1);
    if (!XrOk("grip click", xrCreateAction(g_actions, &action, &g_gripClick))) return 0;

    action.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strncpy(action.actionName, "grip_pose", sizeof(action.actionName) - 1);
    strncpy(action.localizedActionName, "Grip Pose", sizeof(action.localizedActionName) - 1);
    if (!XrOk("grip pose", xrCreateAction(g_actions, &action, &g_pose))) return 0;

    action.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    strncpy(action.actionName, "haptic", sizeof(action.actionName) - 1);
    strncpy(action.localizedActionName, "Haptic", sizeof(action.localizedActionName) - 1);
    if (!XrOk("haptic", xrCreateAction(g_actions, &action, &g_haptic))) return 0;

    SuggestProfile("/interaction_profiles/oculus/touch_controller", "input/squeeze/value", 0);
    SuggestProfile("/interaction_profiles/meta/touch_plus_controller", "input/squeeze/value", 0);
    SuggestProfile("/interaction_profiles/meta/touch_pro_controller", "input/squeeze/value", 0);
    SuggestProfile("/interaction_profiles/valve/index_controller", "input/squeeze/value", 0);
    SuggestProfile("/interaction_profiles/htc/vive_controller", "input/squeeze/click", 1);
    SuggestProfile("/interaction_profiles/microsoft/motion_controller", "input/squeeze/click", 1);
    SuggestProfile("/interaction_profiles/khr/simple_controller", "input/select/click", 1);
    return 1;
}

static int PickColorFormat(void)
{
    uint32_t count = 0;
    if (!XrOk("formats", xrEnumerateSwapchainFormats(g_session, 0, &count, 0)) || count == 0) return 0;
    if (count > 64) count = 64;
    int64_t formats[64];
    if (!XrOk("formats", xrEnumerateSwapchainFormats(g_session, count, &count, formats))) return 0;
    for (uint32_t i = 0; i < count; i++) if (formats[i] == GL_RGBA8) return GL_RGBA8;
    for (uint32_t i = 0; i < count; i++) if (formats[i] == GL_SRGB8_ALPHA8) return GL_SRGB8_ALPHA8;
    return (int)formats[0];
}

static int CreateEye(EyeTarget* eye, const XrViewConfigurationView* view, int format)
{
    memset(eye, 0, sizeof(*eye));
    eye->width = view->recommendedImageRectWidth;
    eye->height = view->recommendedImageRectHeight;

    XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = eye->width;
    info.height = eye->height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!XrOk("swapchain", xrCreateSwapchain(g_session, &info, &eye->swapchain))) return 0;

    uint32_t count = 0;
    xrEnumerateSwapchainImages(eye->swapchain, 0, &count, 0);
    if (count == 0 || count > MAX_SWAP_IMAGES) {
        fprintf(stderr, "[xr] unexpected swapchain image count %u\n", count);
        return 0;
    }
    BalloonXrSwapchainImage images[MAX_SWAP_IMAGES];
    for (uint32_t i = 0; i < count; i++) {
        images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        images[i].next = 0;
        images[i].image = 0;
    }
    if (!XrOk("swapchain images", xrEnumerateSwapchainImages(
            eye->swapchain, count, &count, (XrSwapchainImageBaseHeader*)images))) return 0;
    eye->imageCount = count;
    for (uint32_t i = 0; i < count; i++) eye->images[i] = images[i].image;

    eye->fbo = rlLoadFramebuffer();
    /* A renderbuffer depth attachment matches what raylib uses for its own
     * targets and does not depend on depth-texture support. */
    eye->depth = rlLoadTextureDepth((int)eye->width, (int)eye->height, 1);
    return eye->fbo != 0 && eye->depth != 0;
}

static void DestroyEyes(void)
{
    for (int i = 0; i < g_eyeCount; i++) {
        if (g_eyes[i].depth) glDeleteRenderbuffers(1, &g_eyes[i].depth);
        if (g_eyes[i].fbo) rlUnloadFramebuffer(g_eyes[i].fbo);
        if (g_eyes[i].swapchain) xrDestroySwapchain(g_eyes[i].swapchain);
        g_eyes[i].swapchain = 0;
    }
    g_eyeCount = 0;
}

static Matrix ProjectionForFov(XrFovf fov)
{
    const float nearZ = 0.05f;
    const float farZ = 4000.0f;
    float left = tanf(fov.angleLeft) * nearZ;
    float right = tanf(fov.angleRight) * nearZ;
    float bottom = tanf(fov.angleDown) * nearZ;
    float top = tanf(fov.angleUp) * nearZ;
    return MatrixFrustum(left, right, bottom, top, nearZ, farZ);
}

static Matrix ViewForPose(XrPosef pose, Vector3* outPosition)
{
    Quaternion q = { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w };
    Vector3 position = {
        pose.position.x - g_originX,
        pose.position.y + g_floorLift,
        pose.position.z - g_originZ
    };
    Vector3 forward = Vector3RotateByQuaternion((Vector3){ 0, 0, -1 }, q);
    Vector3 up = Vector3RotateByQuaternion((Vector3){ 0, 1, 0 }, q);
    if (outPosition) *outPosition = position;
    return MatrixLookAt(position, Vector3Add(position, forward), up);
}

static void StoreMatrix(float dest[16], Matrix m)
{
    float16 f = MatrixToFloatV(m);
    memcpy(dest, f.v, sizeof(float) * 16);
}

static void PollEvents(void)
{
    XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
    while (g_instance && xrPollEvent(g_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged* changed = (XrEventDataSessionStateChanged*)&event;
            fprintf(stderr, "[xr] session state %d\n", (int)changed->state);
            switch (changed->state) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XrOk("begin session", xrBeginSession(g_session, &begin))) g_sessionRunning = 1;
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    xrEndSession(g_session);
                    g_sessionRunning = 0;
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    g_wantsQuit = 1;
                    break;
                default:
                    break;
            }
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

static void Haptic(int hand)
{
    if (!g_haptic || !g_session) return;
    XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
    vibration.amplitude = 0.45f;
    vibration.duration = 40000000;
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
    info.action = g_haptic;
    info.subactionPath = g_handPath[hand];
    xrApplyHapticFeedback(g_session, &info, (const XrHapticBaseHeader*)&vibration);
}

static void ReadHands(HandInput hands[2])
{
    XrActiveActionSet active = { g_actions, XR_NULL_PATH };
    XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(g_session, &sync))) {
        hands[0].active = 0;
        hands[1].active = 0;
        return;
    }

    for (int i = 0; i < 2; i++) {
        HandInput* hand = &hands[i];
        memset(hand, 0, sizeof(*hand));
        hand->qw = 1.0f;

        XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
        get.subactionPath = g_handPath[i];

        get.action = g_gripValue;
        XrActionStateFloat value = { XR_TYPE_ACTION_STATE_FLOAT };
        xrGetActionStateFloat(g_session, &get, &value);

        get.action = g_gripClick;
        XrActionStateBoolean click = { XR_TYPE_ACTION_STATE_BOOLEAN };
        xrGetActionStateBoolean(g_session, &get, &click);

        int grip = (value.isActive && value.currentState > 0.55f) || (click.isActive && click.currentState);
        hand->grip = grip;
        if (grip && !g_gripWas[i]) Haptic(i);
        g_gripWas[i] = grip;

        XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
        if (XR_SUCCEEDED(xrLocateSpace(g_handSpace[i], g_space, g_frameState.predictedDisplayTime, &location))) {
            int posOk = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
            int rotOk = (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
            if (posOk) {
                hand->active = 1;
                hand->x = location.pose.position.x - g_originX;
                hand->y = location.pose.position.y + g_floorLift;
                hand->z = location.pose.position.z - g_originZ;
            }
            if (rotOk) {
                hand->qx = location.pose.orientation.x;
                hand->qy = location.pose.orientation.y;
                hand->qz = location.pose.orientation.z;
                hand->qw = location.pose.orientation.w;
            }
        }
    }
}

static void ApplyRecenter(Vector3 rawHead)
{
    if (!g_needRecenter) return;
    g_originX = rawHead.x;
    g_originZ = rawHead.z;
    if (!g_floorChosen) {
        /* STAGE puts the floor at y = 0. A head near y = 0 means the runtime
         * actually gave us a head-origin LOCAL space. */
        if (!g_stage || fabsf(rawHead.y) < 0.40f) g_floorLift = 1.62f;
        else g_floorLift = 0.0f;
        g_floorChosen = 1;
        fprintf(stderr, "[xr] floor lift %.2f m (%s)\n", g_floorLift, g_stage ? "stage" : "local");
    }
    g_needRecenter = 0;
    fprintf(stderr, "[xr] recentered on head (%.2f, %.2f)\n", g_originX, g_originZ);
}

int XR_Init(void)
{
    const char* extensions[] = { "XR_KHR_opengl_enable" };
    XrInstanceCreateInfo instanceInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
    strncpy(instanceInfo.applicationInfo.applicationName, "Balloon VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    instanceInfo.applicationInfo.applicationVersion = 1;
    strncpy(instanceInfo.applicationInfo.engineName, "raylib", XR_MAX_ENGINE_NAME_SIZE - 1);
    instanceInfo.applicationInfo.engineVersion = 1;
    instanceInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = extensions;

    XrResult created = xrCreateInstance(&instanceInfo, &g_instance);
    if (XR_FAILED(created)) {
        fprintf(stderr, "[xr] no OpenXR runtime (%d). Continuing on the desktop.\n", (int)created);
        g_instance = 0;
        return 0;
    }

    XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!XrOk("system", xrGetSystem(g_instance, &systemInfo, &g_system))) {
        XR_Shutdown();
        return 0;
    }

    BalloonPfnGetGlRequirements getReqs = 0;
    xrGetInstanceProcAddr(g_instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReqs);
    if (!getReqs) {
        fprintf(stderr, "[xr] OpenGL enable extension is missing.\n");
        XR_Shutdown();
        return 0;
    }
    BalloonXrGlRequirements requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    if (!XrOk("gl requirements", getReqs(g_instance, g_system, &requirements))) {
        XR_Shutdown();
        return 0;
    }
    int major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    XrVersion have = XR_MAKE_VERSION((unsigned)major, (unsigned)minor, 0);
    fprintf(stderr, "[xr] GL %d.%d, runtime wants %d.%d .. %d.%d\n",
            major, minor,
            (int)XR_VERSION_MAJOR(requirements.minApiVersionSupported),
            (int)XR_VERSION_MINOR(requirements.minApiVersionSupported),
            (int)XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
            (int)XR_VERSION_MINOR(requirements.maxApiVersionSupported));
    if (have < requirements.minApiVersionSupported) {
        fprintf(stderr, "[xr] GL context is older than the runtime requires. Rebuild raylib with OpenGL 4.3.\n");
        XR_Shutdown();
        return 0;
    }

    if (!CreateActions()) {
        XR_Shutdown();
        return 0;
    }

    const void* binding = Glx_GraphicsBinding();
    if (!binding) {
        XR_Shutdown();
        return 0;
    }

    XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = binding;
    sessionInfo.systemId = g_system;
    if (!XrOk("session", xrCreateSession(g_instance, &sessionInfo, &g_session))) {
        XR_Shutdown();
        return 0;
    }

    XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &g_actions;
    if (!XrOk("attach actions", xrAttachSessionActionSets(g_session, &attach))) {
        XR_Shutdown();
        return 0;
    }

    for (int i = 0; i < 2; i++) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = g_pose;
        spaceInfo.subactionPath = g_handPath[i];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!XrOk("hand space", xrCreateActionSpace(g_session, &spaceInfo, &g_handSpace[i]))) {
            XR_Shutdown();
            return 0;
        }
    }

    XrReferenceSpaceCreateInfo space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    space.poseInReferenceSpace.orientation.w = 1.0f;
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (XR_FAILED(xrCreateReferenceSpace(g_session, &space, &g_space))) {
        space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (!XrOk("local space", xrCreateReferenceSpace(g_session, &space, &g_space))) {
            XR_Shutdown();
            return 0;
        }
        g_stage = 0;
        fprintf(stderr, "[xr] reference space: local\n");
    } else {
        g_stage = 1;
        fprintf(stderr, "[xr] reference space: stage (room-scale)\n");
    }

    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, 0);
    if (viewCount == 0 || viewCount > MAX_VIEWS) viewCount = 2;
    XrViewConfigurationView views[MAX_VIEWS];
    for (uint32_t i = 0; i < viewCount; i++) views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    if (!XrOk("views", xrEnumerateViewConfigurationViews(
            g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views))) {
        XR_Shutdown();
        return 0;
    }

    int format = PickColorFormat();
    if (!format) {
        XR_Shutdown();
        return 0;
    }
    g_eyeCount = (int)viewCount;
    for (int i = 0; i < g_eyeCount; i++) {
        if (!CreateEye(&g_eyes[i], &views[i], format)) {
            XR_Shutdown();
            return 0;
        }
        fprintf(stderr, "[xr] eye %d swapchain %ux%u\n", i, g_eyes[i].width, g_eyes[i].height);
    }

    uint32_t blendCount = 0;
    xrEnumerateEnvironmentBlendModes(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blendCount, 0);
    if (blendCount > 0) {
        if (blendCount > 8) blendCount = 8;
        XrEnvironmentBlendMode modes[8];
        xrEnumerateEnvironmentBlendModes(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, blendCount, &blendCount, modes);
        g_blend = modes[0];
        for (uint32_t i = 0; i < blendCount; i++) {
            if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) g_blend = modes[i];
        }
    }

    g_ready = 1;
    fprintf(stderr, "[xr] session created. Put the headset on to begin.\n");
    return 1;
}

void XR_Shutdown(void)
{
    if (g_session && g_sessionRunning) {
        xrRequestExitSession(g_session);
        xrEndSession(g_session);
        g_sessionRunning = 0;
    }
    DestroyEyes();
    for (int i = 0; i < 2; i++) {
        if (g_handSpace[i]) xrDestroySpace(g_handSpace[i]);
        g_handSpace[i] = 0;
    }
    if (g_space) xrDestroySpace(g_space);
    g_space = 0;
    if (g_session) xrDestroySession(g_session);
    g_session = 0;
    if (g_actions) xrDestroyActionSet(g_actions);
    g_actions = 0;
    if (g_instance) xrDestroyInstance(g_instance);
    g_instance = 0;
    g_ready = 0;
}

int XR_IsSessionRunning(void) { return g_sessionRunning; }
int XR_WantsQuit(void) { return g_wantsQuit; }
void XR_Recenter(void) { g_needRecenter = 1; }

int XR_BeginFrame(XrFrame* frame)
{
    memset(frame, 0, sizeof(*frame));
    frame->mirrorFovy = 70.0f;
    frame->mirrorUx = 0.0f;
    frame->mirrorUy = 1.0f;
    frame->mirrorUz = 0.0f;
    frame->hands[0].qw = 1.0f;
    frame->hands[1].qw = 1.0f;
    if (!g_ready) return 0;

    PollEvents();
    if (!g_sessionRunning) return 0;

    XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
    g_frameState.type = XR_TYPE_FRAME_STATE;
    g_frameState.next = 0;
    if (!XrOk("wait frame", xrWaitFrame(g_session, &waitInfo, &g_frameState))) return 0;

    XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
    if (!XrOk("begin frame", xrBeginFrame(g_session, &beginInfo))) return 0;
    g_frameOpen = 1;

    double seconds = (double)g_frameState.predictedDisplayPeriod / 1.0e9;
    frame->dt = (float)seconds;
    if (frame->dt < 0.001f || frame->dt > 0.05f) frame->dt = 1.0f / 72.0f;
    frame->shouldRender = g_frameState.shouldRender ? 1 : 0;
    frame->eyeCount = g_eyeCount;

    if (!frame->shouldRender) {
        ReadHands(frame->hands);
        return 1;
    }

    XrView views[MAX_VIEWS];
    for (int i = 0; i < g_eyeCount; i++) {
        views[i].type = XR_TYPE_VIEW;
        views[i].next = 0;
    }
    XrViewState viewState = { XR_TYPE_VIEW_STATE };
    XrViewLocateInfo locate = { XR_TYPE_VIEW_LOCATE_INFO };
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = g_frameState.predictedDisplayTime;
    locate.space = g_space;
    uint32_t located = 0;
    if (!XrOk("locate views", xrLocateViews(g_session, &locate, &viewState, (uint32_t)g_eyeCount, &located, views))) {
        frame->shouldRender = 0;
        ReadHands(frame->hands);
        return 1;
    }

    Vector3 rawHead = { 0 };
    if (located > 0) {
        rawHead.x = views[0].pose.position.x;
        rawHead.y = views[0].pose.position.y;
        rawHead.z = views[0].pose.position.z;
        if (located > 1) {
            rawHead.x = 0.5f * (rawHead.x + views[1].pose.position.x);
            rawHead.z = 0.5f * (rawHead.z + views[1].pose.position.z);
        }
        ApplyRecenter(rawHead);
    }

    ReadHands(frame->hands);

    Vector3 mirrorPos = { 0 };
    Vector3 mirrorForward = { 0, 0, -1 };
    Vector3 mirrorUp = { 0, 1, 0 };
    for (int i = 0; i < g_eyeCount && i < (int)located; i++) {
        g_eyes[i].pose = views[i].pose;
        g_eyes[i].fov = views[i].fov;
        Vector3 eyePos = { 0 };
        StoreMatrix(g_eyes[i].proj, ProjectionForFov(views[i].fov));
        StoreMatrix(g_eyes[i].view, ViewForPose(views[i].pose, &eyePos));
        frame->eyeX[i] = eyePos.x;
        frame->eyeY[i] = eyePos.y;
        frame->eyeZ[i] = eyePos.z;
        if (i == 0) {
            Quaternion q = { views[i].pose.orientation.x, views[i].pose.orientation.y, views[i].pose.orientation.z, views[i].pose.orientation.w };
            mirrorPos = eyePos;
            mirrorForward = Vector3RotateByQuaternion((Vector3){ 0, 0, -1 }, q);
            mirrorUp = Vector3RotateByQuaternion((Vector3){ 0, 1, 0 }, q);
            float fovy = (views[i].fov.angleUp - views[i].fov.angleDown) * RAD2DEG;
            if (fovy > 20.0f && fovy < 140.0f) frame->mirrorFovy = fovy;
        }
    }
    frame->mirrorPx = mirrorPos.x;
    frame->mirrorPy = mirrorPos.y;
    frame->mirrorPz = mirrorPos.z;
    frame->mirrorTx = mirrorPos.x + mirrorForward.x;
    frame->mirrorTy = mirrorPos.y + mirrorForward.y;
    frame->mirrorTz = mirrorPos.z + mirrorForward.z;
    frame->mirrorUx = mirrorUp.x;
    frame->mirrorUy = mirrorUp.y;
    frame->mirrorUz = mirrorUp.z;
    return 1;
}

void XR_EndFrame(void)
{
    if (!g_frameOpen) return;

    int layerCount = 0;
    const XrCompositionLayerBaseHeader* layers[1];
    if (g_frameState.shouldRender && g_eyeCount > 0) {
        g_layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        g_layer.next = 0;
        g_layer.layerFlags = 0;
        g_layer.space = g_space;
        g_layer.viewCount = (uint32_t)g_eyeCount;
        g_layer.views = g_projViews;
        for (int i = 0; i < g_eyeCount; i++) {
            g_projViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            g_projViews[i].next = 0;
            g_projViews[i].pose = g_eyes[i].pose;
            g_projViews[i].fov = g_eyes[i].fov;
            g_projViews[i].subImage.swapchain = g_eyes[i].swapchain;
            g_projViews[i].subImage.imageRect.offset.x = 0;
            g_projViews[i].subImage.imageRect.offset.y = 0;
            g_projViews[i].subImage.imageRect.extent.width = (int32_t)g_eyes[i].width;
            g_projViews[i].subImage.imageRect.extent.height = (int32_t)g_eyes[i].height;
            g_projViews[i].subImage.imageArrayIndex = 0;
        }
        layers[0] = (const XrCompositionLayerBaseHeader*)&g_layer;
        layerCount = 1;
    }

    XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO };
    end.displayTime = g_frameState.predictedDisplayTime;
    end.environmentBlendMode = g_blend;
    end.layerCount = (uint32_t)layerCount;
    end.layers = layerCount ? layers : 0;
    xrEndFrame(g_session, &end);
    g_frameOpen = 0;
}

void XR_BeginEye(int eye)
{
    if (eye < 0 || eye >= g_eyeCount) return;
    EyeTarget* target = &g_eyes[eye];

    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrResult result = xrAcquireSwapchainImage(target->swapchain, &acquire, &target->imageIndex);
    if (XR_FAILED(result)) {
        XrLogResult("acquire", result);
        return;
    }
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(target->swapchain, &wait);
    target->acquired = 1;

    unsigned int color = target->images[target->imageIndex];
    rlFramebufferAttach(target->fbo, color, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    rlFramebufferAttach(target->fbo, target->depth, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_RENDERBUFFER, 0);

    rlDrawRenderBatchActive();
    rlEnableFramebuffer(target->fbo);
    rlViewport(0, 0, (int)target->width, (int)target->height);
    rlSetFramebufferWidth((int)target->width);
    rlSetFramebufferHeight((int)target->height);
    rlClearColor(135, 196, 222, 255);
    rlClearScreenBuffers();
    rlEnableDepthTest();

    rlMatrixMode(RL_PROJECTION);
    rlPushMatrix();
    rlLoadIdentity();
    rlMultMatrixf(target->proj);
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlMultMatrixf(target->view);
}

void XR_EndEye(int eye)
{
    if (eye < 0 || eye >= g_eyeCount) return;
    EyeTarget* target = &g_eyes[eye];
    rlDrawRenderBatchActive();
    rlMatrixMode(RL_PROJECTION);
    rlPopMatrix();
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlDisableDepthTest();
    rlDisableFramebuffer();
    if (target->acquired) {
        XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(target->swapchain, &release);
        target->acquired = 0;
    }
}
