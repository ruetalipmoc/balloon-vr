/* Current GLX context as an OpenXR graphics binding.
 * Kept in its own translation unit so raylib's GL loader and glx.h
 * do not fight over the same typedefs. */
#ifndef BALLOON_XR_GLX_H
#define BALLOON_XR_GLX_H

/* Pointer to an XrGraphicsBindingOpenGLXlibKHR, or NULL if there is no
 * current GLX context (Wayland-only session, or InitWindow not called). */
const void* Glx_GraphicsBinding(void);

#endif
