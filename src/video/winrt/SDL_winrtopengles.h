/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2014 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_config.h"

#ifndef _SDL_winrtopengles_h
#define _SDL_winrtopengles_h

#if SDL_VIDEO_DRIVER_WINRT && SDL_VIDEO_OPENGL_EGL

#include "../SDL_sysvideo.h"
#include "../SDL_egl_c.h"

/* OpenGLES functions */
#define WINRT_GLES_GetAttribute SDL_EGL_GetAttribute
#define WINRT_GLES_GetProcAddress SDL_EGL_GetProcAddress
#define WINRT_GLES_SetSwapInterval SDL_EGL_SetSwapInterval
#define WINRT_GLES_GetSwapInterval SDL_EGL_GetSwapInterval
#define WINRT_GLES_DeleteContext SDL_EGL_DeleteContext

extern int WINRT_GLES_LoadLibrary(_THIS, const char *path);
extern void WINRT_GLES_UnloadLibrary(_THIS);
extern SDL_GLContext WINRT_GLES_CreateContext(_THIS, SDL_Window * window);
extern void WINRT_GLES_SwapWindow(_THIS, SDL_Window * window);
extern int WINRT_GLES_MakeCurrent(_THIS, SDL_Window * window, SDL_GLContext context);


#ifdef __cplusplus

/* Typedefs for ANGLE/WinRT's C++-based native-display and native-window types,
 * which are used when calling eglGetDisplay and eglCreateWindowSurface.
 */
typedef Microsoft::WRL::ComPtr<IUnknown> WINRT_EGLNativeWindowType;
typedef WINRT_EGLNativeWindowType WINRT_EGLNativeDisplayType;

/* Function pointer typedefs for ANGLE/WinRT's functions that require
 * parameter customization [by passing in C++ objects].
 */
typedef EGLDisplay (EGLAPIENTRY *eglGetDisplay_Function)(WINRT_EGLNativeWindowType);
typedef EGLSurface (EGLAPIENTRY *eglCreateWindowSurface_Function)(EGLDisplay, EGLConfig, WINRT_EGLNativeWindowType, const EGLint *);
typedef HRESULT (EGLAPIENTRY *CreateWinrtEglWindow_Function)(Microsoft::WRL::ComPtr<IUnknown>, int, IUnknown ** result);

#endif /* __cplusplus */

#endif /* SDL_VIDEO_DRIVER_WINRT && SDL_VIDEO_OPENGL_EGL */

#endif /* _SDL_winrtopengles_h */

/* vi: set ts=4 sw=4 expandtab: */
