/* X11 and GLX have to be visible before the OpenXR platform header so the
 * binding struct is compiled with the real Display / GLX types. */
#include <X11/Xlib.h>
#include <GL/glx.h>

#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <stdio.h>

static XrGraphicsBindingOpenGLXlibKHR g_binding;

const void* Glx_GraphicsBinding(void)
{
    Display* display = glXGetCurrentDisplay();
    GLXContext context = glXGetCurrentContext();
    GLXDrawable drawable = glXGetCurrentDrawable();
    if (!display || !context || !drawable) {
        fprintf(stderr, "[xr] No current GLX context. An X11 DISPLAY is required for OpenXR.\n");
        return 0;
    }

    int fbId = 0;
    int query = glXQueryContext(display, context, GLX_FBCONFIG_ID, &fbId);
    if (query != 0 || fbId == 0) {
        fprintf(stderr, "[xr] glXQueryContext(GLX_FBCONFIG_ID) failed (%d).\n", query);
        return 0;
    }

    int count = 0;
    GLXFBConfig* configs = glXGetFBConfigs(display, DefaultScreen(display), &count);
    if (!configs) {
        fprintf(stderr, "[xr] glXGetFBConfigs returned nothing.\n");
        return 0;
    }

    GLXFBConfig match = 0;
    for (int i = 0; i < count; i++) {
        int id = 0;
        glXGetFBConfigAttrib(display, configs[i], GLX_FBCONFIG_ID, &id);
        if (id == fbId) {
            match = configs[i];
            break;
        }
    }
    if (!match) {
        fprintf(stderr, "[xr] No GLXFBConfig matches id %d.\n", fbId);
        XFree(configs);
        return 0;
    }

    int visual = 0;
    glXGetFBConfigAttrib(display, match, GLX_VISUAL_ID, &visual);

    g_binding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR;
    g_binding.next = 0;
    g_binding.xDisplay = display;
    g_binding.visualid = (unsigned int)visual;
    g_binding.glxFBConfig = match;
    g_binding.glxDrawable = drawable;
    g_binding.glxContext = context;

    /* The FBConfig pointer remains valid until the display is closed.
     * The config list itself can be freed; the selected pointer stays. */
    XFree(configs);

    fprintf(stderr, "[xr] GLX binding ready (visual 0x%x, fbconfig %d).\n", visual, fbId);
    return &g_binding;
}
