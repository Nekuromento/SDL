/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2010 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org
*/
#include "SDL_config.h"

/* The high-level video driver subsystem */

#include "SDL.h"
#include "SDL_video.h"
#include "SDL_sysvideo.h"
#include "SDL_blit.h"
#include "SDL_pixels_c.h"
#include "../events/SDL_events_c.h"

#if SDL_VIDEO_OPENGL_ES
#include "SDL_opengles.h"
#endif /* SDL_VIDEO_OPENGL_ES */

#if SDL_VIDEO_OPENGL
#include "SDL_opengl.h"
#endif /* SDL_VIDEO_OPENGL */

#include "SDL_syswm.h"

/* On Windows, windows.h defines CreateWindow */
#ifdef CreateWindow
#undef CreateWindow
#endif

/* Available video drivers */
static VideoBootStrap *bootstrap[] = {
#if SDL_VIDEO_DRIVER_COCOA
    &COCOA_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_X11
    &X11_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_DIRECTFB
    &DirectFB_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_WINDOWS
    &WINDOWS_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_BWINDOW
    &BWINDOW_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_PANDORA
    &PND_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_NDS
    &NDS_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_UIKIT
    &UIKIT_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_ANDROID
    &Android_bootstrap,
#endif
#if SDL_VIDEO_DRIVER_DUMMY
    &DUMMY_bootstrap,
#endif
    NULL
};

static SDL_VideoDevice *_this = NULL;

#define CHECK_WINDOW_MAGIC(window, retval) \
    if (!_this) { \
        SDL_UninitializedVideo(); \
        return retval; \
    } \
    if (!window || window->magic != &_this->window_magic) { \
        SDL_SetError("Invalid window"); \
        return retval; \
    }

/* Various local functions */
static void SDL_UpdateWindowGrab(SDL_Window * window);

/* Support for framebuffer emulation using an accelerated renderer */

#define SDL_WINDOWTEXTUREDATA   "_SDL_WindowTextureData"

typedef struct {
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    void *pixels;
    int pitch;
    int bytes_per_pixel;
} SDL_WindowTextureData;

static SDL_bool
ShouldUseTextureFramebuffer()
{
    const char *hint;

    /* If there's no native framebuffer support then there's no option */
    if (!_this->CreateWindowFramebuffer) {
        return SDL_TRUE;
    }

    /* See if the user or application wants a specific behavior */
    hint = SDL_GetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION);
    if (hint) {
        if (*hint == '0') {
            return SDL_FALSE;
        } else {
            return SDL_TRUE;
        }
    }

    /* Each platform has different performance characteristics */
#if defined(__WIN32__)
    /* GDI BitBlt() is way faster than Direct3D dynamic textures right now.
     */
    return SDL_FALSE;

#elif defined(__MACOSX__)
    /* Mac OS X uses OpenGL as the native fast path */
    return SDL_TRUE;

#elif defined(__LINUX__)
    /* Properly configured OpenGL drivers are faster than MIT-SHM */
#if SDL_VIDEO_OPENGL
    /* Ugh, find a way to cache this value! */
    {
        SDL_Window *window;
        SDL_GLContext context;
        SDL_bool hasAcceleratedOpenGL = SDL_FALSE;

        window = SDL_CreateWindow("OpenGL test", -32, -32, 32, 32, SDL_WINDOW_OPENGL);
        if (window) {
            context = SDL_GL_CreateContext(window);
            if (context) {
                const GLubyte *(APIENTRY * glGetStringFunc) (GLenum);
                const char *vendor = NULL;

                glGetStringFunc = SDL_GL_GetProcAddress("glGetString");
                if (glGetStringFunc) {
                    vendor = (const char *) glGetStringFunc(GL_VENDOR);
                }
                /* Add more vendors here at will... */
                if (vendor &&
                    (SDL_strstr(vendor, "ATI Technologies") ||
                     SDL_strstr(vendor, "NVIDIA"))) {
                    hasAcceleratedOpenGL = SDL_TRUE;
                }
                SDL_GL_DeleteContext(context);
            }
            SDL_DestroyWindow(window);
        }
        return hasAcceleratedOpenGL;
    }
#else
    return SDL_FALSE;
#endif

#else
    /* Play it safe, assume that if there is a framebuffer driver that it's
       optimized for the current platform.
    */
    return SDL_FALSE;
#endif
}

static int
SDL_CreateWindowTexture(_THIS, SDL_Window * window, Uint32 * format, void ** pixels, int *pitch)
{
    SDL_WindowTextureData *data;
    SDL_Renderer *renderer;
    SDL_RendererInfo info;
    Uint32 i;

    data = SDL_GetWindowData(window, SDL_WINDOWTEXTUREDATA);
    if (!data) {
        data = (SDL_WindowTextureData *)SDL_calloc(1, sizeof(*data));
        if (!data) {
            SDL_OutOfMemory();
            return -1;
        }
        SDL_SetWindowData(window, SDL_WINDOWTEXTUREDATA, data);
    }

    renderer = data->renderer;
    if (!renderer) {
        SDL_RendererInfo info;
        int i;

        /* We need to make sure we don't get a software renderer */
        for (i = 0; i < SDL_GetNumRenderDrivers(); ++i) {
            SDL_GetRenderDriverInfo(i, &info);
            if (SDL_strcmp(info.name, "software") != 0) {
                renderer = SDL_CreateRenderer(window, i, 0);
                if (renderer) {
                    break;
                }
            }
        }
        if (!renderer) {
            return -1;
        }
        data->renderer = renderer;
    }

    /* Free any old texture and pixel data */
    if (data->texture) {
        SDL_DestroyTexture(data->texture);
        data->texture = NULL;
    }
    if (data->pixels) {
        SDL_free(data->pixels);
        data->pixels = NULL;
    }

    if (SDL_GetRendererInfo(renderer, &info) < 0) {
        return -1;
    }

    /* Find the first format without an alpha channel */
    *format = info.texture_formats[0];
    for (i = 0; i < info.num_texture_formats; ++i) {
        if (!SDL_ISPIXELFORMAT_ALPHA(info.texture_formats[i])) {
            *format = info.texture_formats[i];
            break;
        }
    }

    data->texture = SDL_CreateTexture(renderer, *format,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      window->w, window->h);
    if (!data->texture) {
        return -1;
    }

    /* Create framebuffer data */
    data->bytes_per_pixel = SDL_BYTESPERPIXEL(*format);
    data->pitch = (((window->w * data->bytes_per_pixel) + 3) & ~3);
    data->pixels = SDL_malloc(window->h * data->pitch);
    if (!data->pixels) {
        SDL_OutOfMemory();
        return -1;
    }

    *pixels = data->pixels;
    *pitch = data->pitch;
    return 0;
}

static int
SDL_UpdateWindowTexture(_THIS, SDL_Window * window, int numrects, SDL_Rect * rects)
{
    SDL_WindowTextureData *data;
#ifdef UPDATE_TEXTURE_SUBRECTS
    void *src, *dst;
    int src_pitch;
    int dst_pitch;
    int i, row, length;
#endif

    data = SDL_GetWindowData(window, SDL_WINDOWTEXTUREDATA);
    if (!data || !data->texture) {
        SDL_SetError("No window texture data");
        return -1;
    }

#ifdef UPDATE_TEXTURE_SUBRECTS
    src_pitch = data->pitch;
    for (i = 0; i < numrects; ++i) {
        src = (void *)((Uint8 *)data->pixels +
                        rects[i].y * src_pitch +
                        rects[i].x * data->bytes_per_pixel);
        if (SDL_LockTexture(data->texture, &rects[i], &dst, &dst_pitch) < 0) {
            return -1;
        }
        length = rects[i].w * data->bytes_per_pixel;
        for (row = rects[i].h; row--; ) {
            SDL_memcpy(dst, src, length);
            src = (Uint8*)src + src_pitch;
            dst = (Uint8*)dst + dst_pitch;
        }
        SDL_UnlockTexture(data->texture);
    }
#else
    if (SDL_UpdateTexture(data->texture, NULL, data->pixels, data->pitch) < 0) {
        return -1;
    }
#endif

    if (SDL_RenderCopy(data->renderer, data->texture, NULL, NULL) < 0) {
        return -1;
    }

    SDL_RenderPresent(data->renderer);
    return 0;
}

static void
SDL_DestroyWindowTexture(_THIS, SDL_Window * window)
{
    SDL_WindowTextureData *data;

    data = SDL_SetWindowData(window, SDL_WINDOWTEXTUREDATA, NULL);
    if (!data) {
        return;
    }
    if (data->texture) {
        SDL_DestroyTexture(data->texture);
    }
    if (data->renderer) {
        SDL_DestroyRenderer(data->renderer);
    }
    if (data->pixels) {
        SDL_free(data->pixels);
    }
    SDL_free(data);
}


static int
cmpmodes(const void *A, const void *B)
{
    SDL_DisplayMode a = *(const SDL_DisplayMode *) A;
    SDL_DisplayMode b = *(const SDL_DisplayMode *) B;

    if (a.w != b.w) {
        return b.w - a.w;
    }
    if (a.h != b.h) {
        return b.h - a.h;
    }
    if (SDL_BITSPERPIXEL(a.format) != SDL_BITSPERPIXEL(b.format)) {
        return SDL_BITSPERPIXEL(b.format) - SDL_BITSPERPIXEL(a.format);
    }
    if (SDL_PIXELLAYOUT(a.format) != SDL_PIXELLAYOUT(b.format)) {
        return SDL_PIXELLAYOUT(b.format) - SDL_PIXELLAYOUT(a.format);
    }
    if (a.refresh_rate != b.refresh_rate) {
        return b.refresh_rate - a.refresh_rate;
    }
    return 0;
}

static void
SDL_UninitializedVideo()
{
    SDL_SetError("Video subsystem has not been initialized");
}

int
SDL_GetNumVideoDrivers(void)
{
    return SDL_arraysize(bootstrap) - 1;
}

const char *
SDL_GetVideoDriver(int index)
{
    if (index >= 0 && index < SDL_GetNumVideoDrivers()) {
        return bootstrap[index]->name;
    }
    return NULL;
}

/*
 * Initialize the video and event subsystems -- determine native pixel format
 */
int
SDL_VideoInit(const char *driver_name)
{
    SDL_VideoDevice *video;
    int index;
    int i;

    /* Check to make sure we don't overwrite '_this' */
    if (_this != NULL) {
        SDL_VideoQuit();
    }

    /* Start the event loop */
    if (SDL_StartEventLoop() < 0 ||
        SDL_KeyboardInit() < 0 ||
        SDL_MouseInit() < 0 ||
        SDL_TouchInit() < 0 ||
        SDL_QuitInit() < 0) {
        return -1;
    }

    /* Select the proper video driver */
    index = 0;
    video = NULL;
    if (driver_name == NULL) {
        driver_name = SDL_getenv("SDL_VIDEODRIVER");
    }
    if (driver_name != NULL) {
        for (i = 0; bootstrap[i]; ++i) {
            if (SDL_strcasecmp(bootstrap[i]->name, driver_name) == 0) {
                video = bootstrap[i]->create(index);
                break;
            }
        }
    } else {
        for (i = 0; bootstrap[i]; ++i) {
            if (bootstrap[i]->available()) {
                video = bootstrap[i]->create(index);
                if (video != NULL) {
                    break;
                }
            }
        }
    }
    if (video == NULL) {
        if (driver_name) {
            SDL_SetError("%s not available", driver_name);
        } else {
            SDL_SetError("No available video device");
        }
        return -1;
    }
    _this = video;
    _this->name = bootstrap[i]->name;
    _this->next_object_id = 1;


    /* Set some very sane GL defaults */
    _this->gl_config.driver_loaded = 0;
    _this->gl_config.dll_handle = NULL;
    _this->gl_config.red_size = 3;
    _this->gl_config.green_size = 3;
    _this->gl_config.blue_size = 2;
    _this->gl_config.alpha_size = 0;
    _this->gl_config.buffer_size = 0;
    _this->gl_config.depth_size = 16;
    _this->gl_config.stencil_size = 0;
    _this->gl_config.double_buffer = 1;
    _this->gl_config.accum_red_size = 0;
    _this->gl_config.accum_green_size = 0;
    _this->gl_config.accum_blue_size = 0;
    _this->gl_config.accum_alpha_size = 0;
    _this->gl_config.stereo = 0;
    _this->gl_config.multisamplebuffers = 0;
    _this->gl_config.multisamplesamples = 0;
    _this->gl_config.retained_backing = 1;
    _this->gl_config.accelerated = -1;  /* accelerated or not, both are fine */
    _this->gl_config.major_version = 2;
    _this->gl_config.minor_version = 1;

    /* Initialize the video subsystem */
    if (_this->VideoInit(_this) < 0) {
        SDL_VideoQuit();
        return -1;
    }

    /* Make sure some displays were added */
    if (_this->num_displays == 0) {
        SDL_SetError("The video driver did not add any displays");
        SDL_VideoQuit();
        return (-1);
    }

    /* Add the renderer framebuffer emulation if desired */
    if (ShouldUseTextureFramebuffer()) {
        _this->CreateWindowFramebuffer = SDL_CreateWindowTexture;
        _this->UpdateWindowFramebuffer = SDL_UpdateWindowTexture;
        _this->DestroyWindowFramebuffer = SDL_DestroyWindowTexture;
    }

    /* We're ready to go! */
    return 0;
}

const char *
SDL_GetCurrentVideoDriver()
{
    if (!_this) {
        SDL_UninitializedVideo();
        return NULL;
    }
    return _this->name;
}

SDL_VideoDevice *
SDL_GetVideoDevice(void)
{
    return _this;
}

int
SDL_AddBasicVideoDisplay(const SDL_DisplayMode * desktop_mode)
{
    SDL_VideoDisplay display;

    SDL_zero(display);
    if (desktop_mode) {
        display.desktop_mode = *desktop_mode;
    }
    display.current_mode = display.desktop_mode;

    return SDL_AddVideoDisplay(&display);
}

int
SDL_AddVideoDisplay(const SDL_VideoDisplay * display)
{
    SDL_VideoDisplay *displays;
    int index = -1;

    displays =
        SDL_realloc(_this->displays,
                    (_this->num_displays + 1) * sizeof(*displays));
    if (displays) {
        index = _this->num_displays++;
        displays[index] = *display;
        displays[index].device = _this;
        _this->displays = displays;
    } else {
        SDL_OutOfMemory();
    }
    return index;
}

int
SDL_GetNumVideoDisplays(void)
{
    if (!_this) {
        SDL_UninitializedVideo();
        return 0;
    }
    return _this->num_displays;
}

int
SDL_GetDisplayBounds(int index, SDL_Rect * rect)
{
    if (!_this) {
        SDL_UninitializedVideo();
        return -1;
    }
    if (index < 0 || index >= _this->num_displays) {
        SDL_SetError("index must be in the range 0 - %d",
                     _this->num_displays - 1);
        return -1;
    }
    if (rect) {
        SDL_VideoDisplay *display = &_this->displays[index];

        if (_this->GetDisplayBounds) {
            if (_this->GetDisplayBounds(_this, display, rect) < 0) {
                return -1;
            }
        } else {
            /* Assume that the displays are left to right */
            if (index == 0) {
                rect->x = 0;
                rect->y = 0;
            } else {
                SDL_GetDisplayBounds(index-1, rect);
                rect->x += rect->w;
            }
            rect->w = display->desktop_mode.w;
            rect->h = display->desktop_mode.h;
        }
    }
    return 0;
}

int
SDL_SelectVideoDisplay(int index)
{
    if (!_this) {
        SDL_UninitializedVideo();
        return (-1);
    }
    if (index < 0 || index >= _this->num_displays) {
        SDL_SetError("index must be in the range 0 - %d",
                     _this->num_displays - 1);
        return -1;
    }
    _this->current_display = index;
    return 0;
}

int
SDL_GetCurrentVideoDisplay(void)
{
    if (!_this) {
        SDL_UninitializedVideo();
        return (-1);
    }
    return _this->current_display;
}

SDL_bool
SDL_AddDisplayMode(SDL_VideoDisplay * display,  const SDL_DisplayMode * mode)
{
    SDL_DisplayMode *modes;
    int i, nmodes;

    /* Make sure we don't already have the mode in the list */
    modes = display->display_modes;
    nmodes = display->num_display_modes;
    for (i = nmodes; i--;) {
        if (SDL_memcmp(mode, &modes[i], sizeof(*mode)) == 0) {
            return SDL_FALSE;
        }
    }

    /* Go ahead and add the new mode */
    if (nmodes == display->max_display_modes) {
        modes =
            SDL_realloc(modes,
                        (display->max_display_modes + 32) * sizeof(*modes));
        if (!modes) {
            return SDL_FALSE;
        }
        display->display_modes = modes;
        display->max_display_modes += 32;
    }
    modes[nmodes] = *mode;
    display->num_display_modes++;

    /* Re-sort video modes */
    SDL_qsort(display->display_modes, display->num_display_modes,
              sizeof(SDL_DisplayMode), cmpmodes);

    return SDL_TRUE;
}

int
SDL_GetNumDisplayModesForDisplay(SDL_VideoDisplay * display)
{
    if (!display->num_display_modes && _this->GetDisplayModes) {
        _this->GetDisplayModes(_this, display);
        SDL_qsort(display->display_modes, display->num_display_modes,
                  sizeof(SDL_DisplayMode), cmpmodes);
    }
    return display->num_display_modes;
}

int
SDL_GetNumDisplayModes()
{
    if (_this) {
        return SDL_GetNumDisplayModesForDisplay(SDL_CurrentDisplay);
    }
    return 0;
}

int
SDL_GetDisplayModeForDisplay(SDL_VideoDisplay * display, int index, SDL_DisplayMode * mode)
{
    if (index < 0 || index >= SDL_GetNumDisplayModesForDisplay(display)) {
        SDL_SetError("index must be in the range of 0 - %d",
                     SDL_GetNumDisplayModesForDisplay(display) - 1);
        return -1;
    }
    if (mode) {
        *mode = display->display_modes[index];
    }
    return 0;
}

int
SDL_GetDisplayMode(int index, SDL_DisplayMode * mode)
{
    return SDL_GetDisplayModeForDisplay(SDL_CurrentDisplay, index, mode);
}

int
SDL_GetDesktopDisplayModeForDisplay(SDL_VideoDisplay * display, SDL_DisplayMode * mode)
{
    if (mode) {
        *mode = display->desktop_mode;
    }
    return 0;
}

int
SDL_GetDesktopDisplayMode(SDL_DisplayMode * mode)
{
    if (!_this) {
        SDL_UninitializedVideo();
        return -1;
    }
    return SDL_GetDesktopDisplayModeForDisplay(SDL_CurrentDisplay, mode);
}

int
SDL_GetCurrentDisplayModeForDisplay(SDL_VideoDisplay * display, SDL_DisplayMode * mode)
{
    if (mode) {
        *mode = display->current_mode;
    }
    return 0;
}

int
SDL_GetCurrentDisplayMode(SDL_DisplayMode * mode)
{
    if (!_this) {
        SDL_UninitializedVideo();
        return -1;
    }
    return SDL_GetCurrentDisplayModeForDisplay(SDL_CurrentDisplay, mode);
}

SDL_DisplayMode *
SDL_GetClosestDisplayModeForDisplay(SDL_VideoDisplay * display,
                                    const SDL_DisplayMode * mode,
                                    SDL_DisplayMode * closest)
{
    Uint32 target_format;
    int target_refresh_rate;
    int i;
    SDL_DisplayMode *current, *match;

    if (!mode || !closest) {
        SDL_SetError("Missing desired mode or closest mode parameter");
        return NULL;
    }

    /* Default to the desktop format */
    if (mode->format) {
        target_format = mode->format;
    } else {
        target_format = display->desktop_mode.format;
    }

    /* Default to the desktop refresh rate */
    if (mode->refresh_rate) {
        target_refresh_rate = mode->refresh_rate;
    } else {
        target_refresh_rate = display->desktop_mode.refresh_rate;
    }

    match = NULL;
    for (i = 0; i < SDL_GetNumDisplayModesForDisplay(display); ++i) {
        current = &display->display_modes[i];

        if (current->w && (current->w < mode->w)) {
            /* Out of sorted modes large enough here */
            break;
        }
        if (current->h && (current->h < mode->h)) {
            if (current->w && (current->w == mode->w)) {
                /* Out of sorted modes large enough here */
                break;
            }
            /* Wider, but not tall enough, due to a different
               aspect ratio. This mode must be skipped, but closer
               modes may still follow. */
            continue;
        }
        if (!match || current->w < match->w || current->h < match->h) {
            match = current;
            continue;
        }
        if (current->format != match->format) {
            /* Sorted highest depth to lowest */
            if (current->format == target_format ||
                (SDL_BITSPERPIXEL(current->format) >=
                 SDL_BITSPERPIXEL(target_format)
                 && SDL_PIXELTYPE(current->format) ==
                 SDL_PIXELTYPE(target_format))) {
                match = current;
            }
            continue;
        }
        if (current->refresh_rate != match->refresh_rate) {
            /* Sorted highest refresh to lowest */
            if (current->refresh_rate >= target_refresh_rate) {
                match = current;
            }
        }
    }
    if (match) {
        if (match->format) {
            closest->format = match->format;
        } else {
            closest->format = mode->format;
        }
        if (match->w && match->h) {
            closest->w = match->w;
            closest->h = match->h;
        } else {
            closest->w = mode->w;
            closest->h = mode->h;
        }
        if (match->refresh_rate) {
            closest->refresh_rate = match->refresh_rate;
        } else {
            closest->refresh_rate = mode->refresh_rate;
        }
        closest->driverdata = match->driverdata;

        /*
         * Pick some reasonable defaults if the app and driver don't
         * care
         */
        if (!closest->format) {
            closest->format = SDL_PIXELFORMAT_RGB888;
        }
        if (!closest->w) {
            closest->w = 640;
        }
        if (!closest->h) {
            closest->h = 480;
        }
        return closest;
    }
    return NULL;
}

SDL_DisplayMode *
SDL_GetClosestDisplayMode(const SDL_DisplayMode * mode,
                          SDL_DisplayMode * closest)
{
    if (!_this) {
        SDL_UninitializedVideo();
        return NULL;
    }
    return SDL_GetClosestDisplayModeForDisplay(SDL_CurrentDisplay, mode, closest);
}

int
SDL_SetDisplayModeForDisplay(SDL_VideoDisplay * display, const SDL_DisplayMode * mode)
{
    SDL_DisplayMode display_mode;
    SDL_DisplayMode current_mode;

    if (mode) {
        display_mode = *mode;

        /* Default to the current mode */
        if (!display_mode.format) {
            display_mode.format = display->current_mode.format;
        }
        if (!display_mode.w) {
            display_mode.w = display->current_mode.w;
        }
        if (!display_mode.h) {
            display_mode.h = display->current_mode.h;
        }
        if (!display_mode.refresh_rate) {
            display_mode.refresh_rate = display->current_mode.refresh_rate;
        }

        /* Get a good video mode, the closest one possible */
        if (!SDL_GetClosestDisplayModeForDisplay(display, &display_mode, &display_mode)) {
            SDL_SetError("No video mode large enough for %dx%d",
                         display_mode.w, display_mode.h);
            return -1;
        }
    } else {
        display_mode = display->desktop_mode;
    }

    /* See if there's anything left to do */
    SDL_GetCurrentDisplayModeForDisplay(display, &current_mode);
    if (SDL_memcmp(&display_mode, &current_mode, sizeof(display_mode)) == 0) {
        return 0;
    }

    /* Actually change the display mode */
    if (!_this->SetDisplayMode) {
        SDL_SetError("Video driver doesn't support changing display mode");
        return -1;
    }
    if (_this->SetDisplayMode(_this, display, &display_mode) < 0) {
        return -1;
    }
    display->current_mode = display_mode;
    return 0;
}

int
SDL_SetWindowDisplayMode(SDL_Window * window, const SDL_DisplayMode * mode)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (mode) {
        window->fullscreen_mode = *mode;
    } else {
        SDL_zero(window->fullscreen_mode);
    }
    return 0;
}

int
SDL_GetWindowDisplayMode(SDL_Window * window, SDL_DisplayMode * mode)
{
    SDL_DisplayMode fullscreen_mode;

    CHECK_WINDOW_MAGIC(window, -1);

    fullscreen_mode = window->fullscreen_mode;
    if (!fullscreen_mode.w) {
        fullscreen_mode.w = window->w;
    }
    if (!fullscreen_mode.h) {
        fullscreen_mode.h = window->h;
    }

    if (!SDL_GetClosestDisplayModeForDisplay(window->display,
                                             &fullscreen_mode,
                                             &fullscreen_mode)) {
        SDL_SetError("Couldn't find display mode match");
        return -1;
    }

    if (mode) {
        *mode = fullscreen_mode;
    }
    return 0;
}

Uint32
SDL_GetWindowPixelFormat(SDL_Window * window)
{
    SDL_VideoDisplay *display = window->display;
    SDL_DisplayMode *displayMode = &display->current_mode;
    return displayMode->format;
}

static void
SDL_UpdateFullscreenMode(SDL_Window * window, SDL_bool attempt)
{
    SDL_VideoDisplay *display = window->display;

    /* See if we're already processing a window */
    if (display->updating_fullscreen) {
        return;
    }

    display->updating_fullscreen = SDL_TRUE;

    /* See if we even want to do anything here */
    if ((window->flags & SDL_WINDOW_FULLSCREEN) &&
        (window->flags & SDL_WINDOW_SHOWN)) {
        if (attempt) {
            /* We just gained some state, try to gain all states */
            if (window->flags & SDL_WINDOW_MINIMIZED) {
                SDL_RestoreWindow(window);
            } else {
                SDL_RaiseWindow(window);
            }
        } else {
            /* We just lost some state, try to release all states */
            SDL_MinimizeWindow(window);
        }
    }

    if (FULLSCREEN_VISIBLE(window)) {
        /* Hide any other fullscreen windows */
        SDL_Window *other;
        for (other = display->windows; other; other = other->next) {
            if (other != window && FULLSCREEN_VISIBLE(other)) {
                SDL_MinimizeWindow(other);
            }
        }
    }

    display->updating_fullscreen = SDL_FALSE;

    /* See if there are any fullscreen windows */
    for (window = display->windows; window; window = window->next) {
        if (FULLSCREEN_VISIBLE(window)) {
            SDL_DisplayMode fullscreen_mode;
            if (SDL_GetWindowDisplayMode(window, &fullscreen_mode) == 0) {
                SDL_SetDisplayModeForDisplay(display, &fullscreen_mode);
                display->fullscreen_window = window;
                return;
            }
        }
    }

    /* Nope, restore the desktop mode */
    SDL_SetDisplayModeForDisplay(display, NULL);
    display->fullscreen_window = NULL;
}

SDL_Window *
SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    const Uint32 allowed_flags = (SDL_WINDOW_FULLSCREEN |
                                  SDL_WINDOW_OPENGL |
                                  SDL_WINDOW_BORDERLESS |
                                  SDL_WINDOW_RESIZABLE |
                                  SDL_WINDOW_INPUT_GRABBED);
    SDL_VideoDisplay *display;
    SDL_Window *window;

    if (!_this) {
        /* Initialize the video system if needed */
        if (SDL_VideoInit(NULL) < 0) {
            return NULL;
        }
    }
    if (flags & SDL_WINDOW_OPENGL) {
        if (!_this->GL_CreateContext) {
            SDL_SetError("No OpenGL support in video driver");
            return NULL;
        }
        SDL_GL_LoadLibrary(NULL);
    }
    display = SDL_CurrentDisplay;
    window = (SDL_Window *)SDL_calloc(1, sizeof(*window));
    window->magic = &_this->window_magic;
    window->id = _this->next_object_id++;
    window->x = x;
    window->y = y;
    window->w = w;
    window->h = h;
    window->flags = (flags & allowed_flags);
    window->display = display;
    window->next = display->windows;
    if (display->windows) {
        display->windows->prev = window;
    }
    display->windows = window;

    if (_this->CreateWindow && _this->CreateWindow(_this, window) < 0) {
        SDL_DestroyWindow(window);
        return NULL;
    }

    if (title) {
        SDL_SetWindowTitle(window, title);
    }
    if (flags & SDL_WINDOW_MAXIMIZED) {
        SDL_MaximizeWindow(window);
    }
    if (flags & SDL_WINDOW_MINIMIZED) {
        SDL_MinimizeWindow(window);
    }
    if (flags & SDL_WINDOW_SHOWN) {
        SDL_ShowWindow(window);
    }
    SDL_UpdateWindowGrab(window);

    return window;
}

SDL_Window *
SDL_CreateWindowFrom(const void *data)
{
    SDL_VideoDisplay *display;
    SDL_Window *window;

    if (!_this) {
        SDL_UninitializedVideo();
        return NULL;
    }
    display = SDL_CurrentDisplay;
    window = (SDL_Window *)SDL_calloc(1, sizeof(*window));
    window->magic = &_this->window_magic;
    window->id = _this->next_object_id++;
    window->flags = SDL_WINDOW_FOREIGN;
    window->display = display;
    window->next = display->windows;
    if (display->windows) {
        display->windows->prev = window;
    }
    display->windows = window;

    if (!_this->CreateWindowFrom ||
        _this->CreateWindowFrom(_this, window, data) < 0) {
        SDL_DestroyWindow(window);
        return NULL;
    }
    return window;
}

int
SDL_RecreateWindow(SDL_Window * window, Uint32 flags)
{
    const Uint32 allowed_flags = (SDL_WINDOW_FULLSCREEN |
                                  SDL_WINDOW_OPENGL |
                                  SDL_WINDOW_BORDERLESS |
                                  SDL_WINDOW_RESIZABLE |
                                  SDL_WINDOW_INPUT_GRABBED |
                                  SDL_WINDOW_FOREIGN);
    char *title = window->title;

    if ((flags & SDL_WINDOW_OPENGL) && !_this->GL_CreateContext) {
        SDL_SetError("No OpenGL support in video driver");
        return -1;
    }
    if ((window->flags & SDL_WINDOW_OPENGL) != (flags & SDL_WINDOW_OPENGL)) {
        if (flags & SDL_WINDOW_OPENGL) {
            SDL_GL_LoadLibrary(NULL);
        } else {
            SDL_GL_UnloadLibrary();
        }
    }

    if (window->flags & SDL_WINDOW_FOREIGN) {
        /* Can't destroy and re-create foreign windows, hrm */
        flags |= SDL_WINDOW_FOREIGN;
    } else {
        flags &= ~SDL_WINDOW_FOREIGN;
    }

    if (_this->DestroyWindow && !(flags & SDL_WINDOW_FOREIGN)) {
        _this->DestroyWindow(_this, window);
    }

    window->title = NULL;
    window->flags = (flags & allowed_flags);

    if (_this->CreateWindow && !(flags & SDL_WINDOW_FOREIGN)) {
        if (_this->CreateWindow(_this, window) < 0) {
            if (flags & SDL_WINDOW_OPENGL) {
                SDL_GL_UnloadLibrary();
            }
            return -1;
        }
    }

    if (title) {
        SDL_SetWindowTitle(window, title);
        SDL_free(title);
    }
    if (flags & SDL_WINDOW_MAXIMIZED) {
        SDL_MaximizeWindow(window);
    }
    if (flags & SDL_WINDOW_MINIMIZED) {
        SDL_MinimizeWindow(window);
    }
    if (flags & SDL_WINDOW_SHOWN) {
        SDL_ShowWindow(window);
    }
    SDL_UpdateWindowGrab(window);

    return 0;
}

Uint32
SDL_GetWindowID(SDL_Window * window)
{
    CHECK_WINDOW_MAGIC(window, 0);

    return window->id;
}

SDL_Window *
SDL_GetWindowFromID(Uint32 id)
{
    SDL_Window *window;
    int i;

    if (!_this) {
        return NULL;
    }
    /* FIXME: Should we keep a separate hash table for these? */
    for (i = _this->num_displays; i--;) {
        SDL_VideoDisplay *display = &_this->displays[i];
        for (window = display->windows; window; window = window->next) {
            if (window->id == id) {
                return window;
            }
        }
    }
    return NULL;
}

Uint32
SDL_GetWindowFlags(SDL_Window * window)
{
    CHECK_WINDOW_MAGIC(window, 0);

    return window->flags;
}

void
SDL_SetWindowTitle(SDL_Window * window, const char *title)
{
    CHECK_WINDOW_MAGIC(window, );

    if (title == window->title) {
        return;
    }
    if (window->title) {
        SDL_free(window->title);
    }
    if (title && *title) {
        window->title = SDL_strdup(title);
    } else {
        window->title = NULL;
    }

    if (_this->SetWindowTitle) {
        _this->SetWindowTitle(_this, window);
    }
}

const char *
SDL_GetWindowTitle(SDL_Window * window)
{
    CHECK_WINDOW_MAGIC(window, "");

    return window->title ? window->title : "";
}

void
SDL_SetWindowIcon(SDL_Window * window, SDL_Surface * icon)
{
    CHECK_WINDOW_MAGIC(window, );

    if (_this->SetWindowIcon) {
        _this->SetWindowIcon(_this, window, icon);
    }
}

void*
SDL_SetWindowData(SDL_Window * window, const char *name, void *userdata)
{
    SDL_WindowUserData *prev, *data;

    CHECK_WINDOW_MAGIC(window, NULL);

    /* See if the named data already exists */
    prev = NULL;
    for (data = window->data; data; prev = data, data = data->next) {
        if (SDL_strcmp(data->name, name) == 0) {
            void *last_value = data->data;

            if (userdata) {
                /* Set the new value */
                data->data = userdata;
            } else {
                /* Delete this value */
                if (prev) {
                    prev->next = data->next;
                } else {
                    window->data = data->next;
                }
                SDL_free(data->name);
                SDL_free(data);
            }
            return last_value;
        }
    }

    /* Add new data to the window */
    if (userdata) {
        data = (SDL_WindowUserData *)SDL_malloc(sizeof(*data));
        data->name = SDL_strdup(name);
        data->data = userdata;
        data->next = window->data;
        window->data = data;
    }
    return NULL;
}

void *
SDL_GetWindowData(SDL_Window * window, const char *name)
{
    SDL_WindowUserData *data;

    CHECK_WINDOW_MAGIC(window, NULL);

    for (data = window->data; data; data = data->next) {
        if (SDL_strcmp(data->name, name) == 0) {
            return data->data;
        }
    }
    return NULL;
}

void
SDL_SetWindowPosition(SDL_Window * window, int x, int y)
{
    CHECK_WINDOW_MAGIC(window, );

    if (x != SDL_WINDOWPOS_UNDEFINED) {
        window->x = x;
    }
    if (y != SDL_WINDOWPOS_UNDEFINED) {
        window->y = y;
    }
    if (_this->SetWindowPosition) {
        _this->SetWindowPosition(_this, window);
    }
    SDL_SendWindowEvent(window, SDL_WINDOWEVENT_MOVED, x, y);
}

void
SDL_GetWindowPosition(SDL_Window * window, int *x, int *y)
{
    CHECK_WINDOW_MAGIC(window, );

    if (x) {
        *x = window->x;
    }
    if (y) {
        *y = window->y;
    }
}

void
SDL_SetWindowSize(SDL_Window * window, int w, int h)
{
    CHECK_WINDOW_MAGIC(window, );

    window->w = w;
    window->h = h;

    if (_this->SetWindowSize) {
        _this->SetWindowSize(_this, window);
    }
    SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESIZED, w, h);
}

void
SDL_GetWindowSize(SDL_Window * window, int *w, int *h)
{
    if (window) {
        if (w) {
            *w = window->w;
        }
        if (h) {
            *h = window->h;
        }
    } else {
        if (w) {
            *w = 0;
        }
        if (h) {
            *h = 0;
        }
    }
}

void
SDL_ShowWindow(SDL_Window * window)
{
    CHECK_WINDOW_MAGIC(window, );

    if (window->flags & SDL_WINDOW_SHOWN) {
        return;
    }

    if (_this->ShowWindow) {
        _this->ShowWindow(_this, window);
    }
    SDL_SendWindowEvent(window, SDL_WINDOWEVENT_SHOWN, 0, 0);
}

void
SDL_HideWindow(SDL_Window * window)
{
    CHECK_WINDOW_MAGIC(window, );

    if (!(window->flags & SDL_WINDOW_SHOWN)) {
        return;
    }

    if (_this->HideWindow) {
        _this->HideWindow(_this, window);
    }
    SDL_SendWindowEvent(window, SDL_WINDOWEVENT_HIDDEN, 0, 0);
}

void
SDL_RaiseWindow(SDL_Window * window)
{
    CHECK_WINDOW_MAGIC(window, );

    if (!(window->flags & SDL_WINDOW_SHOWN)) {
        return;
    }
    if (_this->RaiseWindow) {
        _this->RaiseWindow(_this, window);
    } else {
        /* FIXME: What we really want is a way to request focus */
        SDL_SendWindowEvent(window, SDL_WINDOWEVENT_FOCUS_GAINED, 0, 0);
    }
}

void
SDL_MaximizeWindow(SDL_Window * window)
{
    CHECK_WINDOW_MAGIC(window, );

    if (window->flags & SDL_WINDOW_MAXIMIZED) {
        return;
    }

    if (_this->MaximizeWindow) {
        _this->MaximizeWindow(_this, window);
    }
    SDL_SendWindowEvent(window, SDL_WINDOWEVENT_MAXIMIZED, 0, 0);
}

void
SDL_MinimizeWindow(SDL_Window * window)
{
    CHECK_WINDOW_MAGIC(window, );

    if (window->flags & SDL_WINDOW_MINIMIZED) {
        return;
    }

    if (_this->MinimizeWindow) {
        _this->MinimizeWindow(_this, window);
    }
    SDL_SendWindowEvent(window, SDL_WINDOWEVENT_MINIMIZED, 0, 0);
}

void
SDL_RestoreWindow(SDL_Window * window)
{
    CHECK_WINDOW_MAGIC(window, );

    if (!(window->flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) {
        return;
    }

    if (_this->RestoreWindow) {
        _this->RestoreWindow(_this, window);
    }
    SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESTORED, 0, 0);
}

int
SDL_SetWindowFullscreen(SDL_Window * window, int fullscreen)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (fullscreen) {
        fullscreen = SDL_WINDOW_FULLSCREEN;
    }
    if ((window->flags & SDL_WINDOW_FULLSCREEN) == fullscreen) {
        return 0;
    }
    if (fullscreen) {
        window->flags |= SDL_WINDOW_FULLSCREEN;

        SDL_UpdateFullscreenMode(window, SDL_TRUE);
    } else {
        window->flags &= ~SDL_WINDOW_FULLSCREEN;

        SDL_UpdateFullscreenMode(window, SDL_FALSE);
    }
    return 0;
}

static SDL_Surface *
SDL_CreateWindowFramebuffer(SDL_Window * window)
{
    Uint32 format;
    void *pixels;
    int pitch;
    int bpp;
    Uint32 Rmask, Gmask, Bmask, Amask;

    if (!_this->CreateWindowFramebuffer || !_this->UpdateWindowFramebuffer) {
        return NULL;
    }

    if (_this->CreateWindowFramebuffer(_this, window, &format, &pixels, &pitch) < 0) {
        return NULL;
    }

    if (!SDL_PixelFormatEnumToMasks(format, &bpp, &Rmask, &Gmask, &Bmask, &Amask)) {
        return NULL;
    }

    return SDL_CreateRGBSurfaceFrom(pixels, window->w, window->h, bpp, pitch, Rmask, Gmask, Bmask, Amask);
}

SDL_Surface *
SDL_GetWindowSurface(SDL_Window * window)
{
    CHECK_WINDOW_MAGIC(window, NULL);

    if (!window->surface_valid) {
        if (window->surface) {
            window->surface->refcount = 0;
            SDL_FreeSurface(window->surface);
        }
        window->surface = SDL_CreateWindowFramebuffer(window);
        if (window->surface) {
            window->surface_valid = SDL_TRUE;
            window->surface->refcount = 0x7FFFFFF;
        }
    }
    return window->surface;
}

int
SDL_UpdateWindowSurface(SDL_Window * window)
{
    SDL_Rect full_rect;

    CHECK_WINDOW_MAGIC(window, -1);

    full_rect.x = 0;
    full_rect.y = 0;
    full_rect.w = window->w;
    full_rect.h = window->h;
    return SDL_UpdateWindowSurfaceRects(window, 1, &full_rect);
}

int
SDL_UpdateWindowSurfaceRects(SDL_Window * window,
                             int numrects, SDL_Rect * rects)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (!window->surface_valid) {
        SDL_SetError("Window surface is invalid, please call SDL_GetWindowSurface() to get a new surface");
        return -1;
    }

    return _this->UpdateWindowFramebuffer(_this, window, numrects, rects);
}

void
SDL_SetWindowGrab(SDL_Window * window, int mode)
{
    CHECK_WINDOW_MAGIC(window, );

    if ((!!mode == !!(window->flags & SDL_WINDOW_INPUT_GRABBED))) {
        return;
    }
    if (mode) {
        window->flags |= SDL_WINDOW_INPUT_GRABBED;
    } else {
        window->flags &= ~SDL_WINDOW_INPUT_GRABBED;
    }
    SDL_UpdateWindowGrab(window);
}

static void
SDL_UpdateWindowGrab(SDL_Window * window)
{
    if ((window->flags & SDL_WINDOW_INPUT_FOCUS) && _this->SetWindowGrab) {
        _this->SetWindowGrab(_this, window);
    }
}

int
SDL_GetWindowGrab(SDL_Window * window)
{
    CHECK_WINDOW_MAGIC(window, 0);

    return ((window->flags & SDL_WINDOW_INPUT_GRABBED) != 0);
}

void
SDL_OnWindowShown(SDL_Window * window)
{
    SDL_RaiseWindow(window);
    SDL_UpdateFullscreenMode(window, SDL_TRUE);
}

void
SDL_OnWindowHidden(SDL_Window * window)
{
    SDL_UpdateFullscreenMode(window, SDL_FALSE);
}

void
SDL_OnWindowResized(SDL_Window * window)
{
    window->surface_valid = SDL_FALSE;
}

void
SDL_OnWindowMinimized(SDL_Window * window)
{
    SDL_UpdateFullscreenMode(window, SDL_FALSE);
}

void
SDL_OnWindowRestored(SDL_Window * window)
{
    SDL_RaiseWindow(window);
    SDL_UpdateFullscreenMode(window, SDL_TRUE);
}

void
SDL_OnWindowFocusGained(SDL_Window * window)
{
    SDL_VideoDisplay *display = window->display;

    if (display->gamma && _this->SetDisplayGammaRamp) {
        _this->SetDisplayGammaRamp(_this, display, display->gamma);
    }
    if ((window->flags & (SDL_WINDOW_INPUT_GRABBED | SDL_WINDOW_FULLSCREEN))
        && _this->SetWindowGrab) {
        _this->SetWindowGrab(_this, window);
    }
}

void
SDL_OnWindowFocusLost(SDL_Window * window)
{
    SDL_VideoDisplay *display = window->display;

    /* If we're fullscreen on a single-head system and lose focus, minimize */
    if ((window->flags & SDL_WINDOW_FULLSCREEN) &&
        _this->num_displays == 1) {
        SDL_MinimizeWindow(window);
    }

    if (display->gamma && _this->SetDisplayGammaRamp) {
        _this->SetDisplayGammaRamp(_this, display, display->saved_gamma);
    }
    if ((window->flags & (SDL_WINDOW_INPUT_GRABBED | SDL_WINDOW_FULLSCREEN))
        && _this->SetWindowGrab) {
        _this->SetWindowGrab(_this, window);
    }
}

SDL_Window *
SDL_GetFocusWindow(void)
{
    SDL_VideoDisplay *display;
    SDL_Window *window;

    if (!_this) {
        return NULL;
    }
    display = SDL_CurrentDisplay;
    for (window = display->windows; window; window = window->next) {
        if (window->flags & SDL_WINDOW_INPUT_FOCUS) {
            return window;
        }
    }
    return NULL;
}

void
SDL_DestroyWindow(SDL_Window * window)
{
    SDL_VideoDisplay *display;

    CHECK_WINDOW_MAGIC(window, );

    /* Restore video mode, etc. */
    SDL_UpdateFullscreenMode(window, SDL_FALSE);

    if (window->surface) {
        window->surface->refcount = 0;
        SDL_FreeSurface(window->surface);
    }
    if (_this->DestroyWindowFramebuffer) {
        _this->DestroyWindowFramebuffer(_this, window);
    }
    if (_this->DestroyWindow) {
        _this->DestroyWindow(_this, window);
    }
    if (window->flags & SDL_WINDOW_OPENGL) {
        SDL_GL_UnloadLibrary();
    }

    /* Now invalidate magic */
    window->magic = NULL;

    /* Free memory associated with the window */
    if (window->title) {
        SDL_free(window->title);
    }
    while (window->data) {
        SDL_WindowUserData *data = window->data;

        window->data = data->next;
        SDL_free(data->name);
        SDL_free(data);
    }

    /* Unlink the window from the list */
    display = window->display;
    if (window->next) {
        window->next->prev = window->prev;
    }
    if (window->prev) {
        window->prev->next = window->next;
    } else {
        display->windows = window->next;
    }

    SDL_free(window);
}

SDL_bool
SDL_IsScreenSaverEnabled()
{
    if (!_this) {
        return SDL_TRUE;
    }
    return _this->suspend_screensaver ? SDL_FALSE : SDL_TRUE;
}

void
SDL_EnableScreenSaver()
{
    if (!_this) {
        return;
    }
    if (!_this->suspend_screensaver) {
        return;
    }
    _this->suspend_screensaver = SDL_FALSE;
    if (_this->SuspendScreenSaver) {
        _this->SuspendScreenSaver(_this);
    }
}

void
SDL_DisableScreenSaver()
{
    if (!_this) {
        return;
    }
    if (_this->suspend_screensaver) {
        return;
    }
    _this->suspend_screensaver = SDL_TRUE;
    if (_this->SuspendScreenSaver) {
        _this->SuspendScreenSaver(_this);
    }
}

void
SDL_VideoQuit(void)
{
    int i, j;

    if (!_this) {
        return;
    }

    /* Halt event processing before doing anything else */
    SDL_QuitQuit();
    SDL_MouseQuit();
    SDL_KeyboardQuit();
    SDL_StopEventLoop();

    SDL_EnableScreenSaver();

    /* Clean up the system video */
    for (i = _this->num_displays; i--;) {
        SDL_VideoDisplay *display = &_this->displays[i];
        while (display->windows) {
            SDL_DestroyWindow(display->windows);
        }
    }
    _this->VideoQuit(_this);

    for (i = _this->num_displays; i--;) {
        SDL_VideoDisplay *display = &_this->displays[i];
        for (j = display->num_display_modes; j--;) {
            if (display->display_modes[j].driverdata) {
                SDL_free(display->display_modes[j].driverdata);
                display->display_modes[j].driverdata = NULL;
            }
        }
        if (display->display_modes) {
            SDL_free(display->display_modes);
            display->display_modes = NULL;
        }
        if (display->desktop_mode.driverdata) {
            SDL_free(display->desktop_mode.driverdata);
            display->desktop_mode.driverdata = NULL;
        }
        if (display->gamma) {
            SDL_free(display->gamma);
            display->gamma = NULL;
        }
        if (display->driverdata) {
            SDL_free(display->driverdata);
            display->driverdata = NULL;
        }
    }
    if (_this->displays) {
        SDL_free(_this->displays);
        _this->displays = NULL;
    }
    if (_this->clipboard_text) {
        SDL_free(_this->clipboard_text);
        _this->clipboard_text = NULL;
    }
    _this->free(_this);
    _this = NULL;
}

int
SDL_GL_LoadLibrary(const char *path)
{
    int retval;

    if (!_this) {
        SDL_UninitializedVideo();
        return -1;
    }
    if (_this->gl_config.driver_loaded) {
        if (path && SDL_strcmp(path, _this->gl_config.driver_path) != 0) {
            SDL_SetError("OpenGL library already loaded");
            return -1;
        }
        retval = 0;
    } else {
        if (!_this->GL_LoadLibrary) {
            SDL_SetError("No dynamic GL support in video driver");
            return -1;
        }
        retval = _this->GL_LoadLibrary(_this, path);
    }
    if (retval == 0) {
        ++_this->gl_config.driver_loaded;
    }
    return (retval);
}

void *
SDL_GL_GetProcAddress(const char *proc)
{
    void *func;

    if (!_this) {
        SDL_UninitializedVideo();
        return NULL;
    }
    func = NULL;
    if (_this->GL_GetProcAddress) {
        if (_this->gl_config.driver_loaded) {
            func = _this->GL_GetProcAddress(_this, proc);
        } else {
            SDL_SetError("No GL driver has been loaded");
        }
    } else {
        SDL_SetError("No dynamic GL support in video driver");
    }
    return func;
}

void
SDL_GL_UnloadLibrary(void)
{
    if (!_this) {
        SDL_UninitializedVideo();
        return;
    }
    if (_this->gl_config.driver_loaded > 0) {
        if (--_this->gl_config.driver_loaded > 0) {
            return;
        }
        if (_this->GL_UnloadLibrary) {
            _this->GL_UnloadLibrary(_this);
        }
    }
}

SDL_bool
SDL_GL_ExtensionSupported(const char *extension)
{
#if SDL_VIDEO_OPENGL || SDL_VIDEO_OPENGL_ES
    const GLubyte *(APIENTRY * glGetStringFunc) (GLenum);
    const char *extensions;
    const char *start;
    const char *where, *terminator;

    /* Extension names should not have spaces. */
    where = SDL_strchr(extension, ' ');
    if (where || *extension == '\0') {
        return SDL_FALSE;
    }
    /* See if there's an environment variable override */
    start = SDL_getenv(extension);
    if (start && *start == '0') {
        return SDL_FALSE;
    }
    /* Lookup the available extensions */
    glGetStringFunc = SDL_GL_GetProcAddress("glGetString");
    if (glGetStringFunc) {
        extensions = (const char *) glGetStringFunc(GL_EXTENSIONS);
    } else {
        extensions = NULL;
    }
    if (!extensions) {
        return SDL_FALSE;
    }
    /*
     * It takes a bit of care to be fool-proof about parsing the OpenGL
     * extensions string. Don't be fooled by sub-strings, etc.
     */

    start = extensions;

    for (;;) {
        where = SDL_strstr(start, extension);
        if (!where)
            break;

        terminator = where + SDL_strlen(extension);
        if (where == start || *(where - 1) == ' ')
            if (*terminator == ' ' || *terminator == '\0')
                return SDL_TRUE;

        start = terminator;
    }
    return SDL_FALSE;
#else
    return SDL_FALSE;
#endif
}

int
SDL_GL_SetAttribute(SDL_GLattr attr, int value)
{
#if SDL_VIDEO_OPENGL || SDL_VIDEO_OPENGL_ES
    int retval;

    if (!_this) {
        SDL_UninitializedVideo();
        return -1;
    }
    retval = 0;
    switch (attr) {
    case SDL_GL_RED_SIZE:
        _this->gl_config.red_size = value;
        break;
    case SDL_GL_GREEN_SIZE:
        _this->gl_config.green_size = value;
        break;
    case SDL_GL_BLUE_SIZE:
        _this->gl_config.blue_size = value;
        break;
    case SDL_GL_ALPHA_SIZE:
        _this->gl_config.alpha_size = value;
        break;
    case SDL_GL_DOUBLEBUFFER:
        _this->gl_config.double_buffer = value;
        break;
    case SDL_GL_BUFFER_SIZE:
        _this->gl_config.buffer_size = value;
        break;
    case SDL_GL_DEPTH_SIZE:
        _this->gl_config.depth_size = value;
        break;
    case SDL_GL_STENCIL_SIZE:
        _this->gl_config.stencil_size = value;
        break;
    case SDL_GL_ACCUM_RED_SIZE:
        _this->gl_config.accum_red_size = value;
        break;
    case SDL_GL_ACCUM_GREEN_SIZE:
        _this->gl_config.accum_green_size = value;
        break;
    case SDL_GL_ACCUM_BLUE_SIZE:
        _this->gl_config.accum_blue_size = value;
        break;
    case SDL_GL_ACCUM_ALPHA_SIZE:
        _this->gl_config.accum_alpha_size = value;
        break;
    case SDL_GL_STEREO:
        _this->gl_config.stereo = value;
        break;
    case SDL_GL_MULTISAMPLEBUFFERS:
        _this->gl_config.multisamplebuffers = value;
        break;
    case SDL_GL_MULTISAMPLESAMPLES:
        _this->gl_config.multisamplesamples = value;
        break;
    case SDL_GL_ACCELERATED_VISUAL:
        _this->gl_config.accelerated = value;
        break;
    case SDL_GL_RETAINED_BACKING:
        _this->gl_config.retained_backing = value;
        break;
    case SDL_GL_CONTEXT_MAJOR_VERSION:
        _this->gl_config.major_version = value;
        break;
    case SDL_GL_CONTEXT_MINOR_VERSION:
        _this->gl_config.minor_version = value;
        break;
    default:
        SDL_SetError("Unknown OpenGL attribute");
        retval = -1;
        break;
    }
    return retval;
#else
    SDL_Unsupported();
    return -1;
#endif /* SDL_VIDEO_OPENGL */
}

int
SDL_GL_GetAttribute(SDL_GLattr attr, int *value)
{
#if SDL_VIDEO_OPENGL || SDL_VIDEO_OPENGL_ES
    void (APIENTRY * glGetIntegervFunc) (GLenum pname, GLint * params);
    GLenum(APIENTRY * glGetErrorFunc) (void);
    GLenum attrib = 0;
    GLenum error = 0;

    glGetIntegervFunc = SDL_GL_GetProcAddress("glGetIntegerv");
    if (!glGetIntegervFunc) {
        return -1;
    }

    glGetErrorFunc = SDL_GL_GetProcAddress("glGetError");
    if (!glGetErrorFunc) {
        return -1;
    }

    /* Clear value in any case */
    *value = 0;

    switch (attr) {
    case SDL_GL_RETAINED_BACKING:
        *value = _this->gl_config.retained_backing;
        return 0;
    case SDL_GL_RED_SIZE:
        attrib = GL_RED_BITS;
        break;
    case SDL_GL_BLUE_SIZE:
        attrib = GL_BLUE_BITS;
        break;
    case SDL_GL_GREEN_SIZE:
        attrib = GL_GREEN_BITS;
        break;
    case SDL_GL_ALPHA_SIZE:
        attrib = GL_ALPHA_BITS;
        break;
    case SDL_GL_DOUBLEBUFFER:
#ifndef SDL_VIDEO_OPENGL_ES
        attrib = GL_DOUBLEBUFFER;
        break;
#else
        /* OpenGL ES 1.0 and above specifications have EGL_SINGLE_BUFFER      */
        /* parameter which switches double buffer to single buffer. OpenGL ES */
        /* SDL driver must set proper value after initialization              */
        *value = _this->gl_config.double_buffer;
        return 0;
#endif
    case SDL_GL_DEPTH_SIZE:
        attrib = GL_DEPTH_BITS;
        break;
    case SDL_GL_STENCIL_SIZE:
        attrib = GL_STENCIL_BITS;
        break;
#ifndef SDL_VIDEO_OPENGL_ES
    case SDL_GL_ACCUM_RED_SIZE:
        attrib = GL_ACCUM_RED_BITS;
        break;
    case SDL_GL_ACCUM_GREEN_SIZE:
        attrib = GL_ACCUM_GREEN_BITS;
        break;
    case SDL_GL_ACCUM_BLUE_SIZE:
        attrib = GL_ACCUM_BLUE_BITS;
        break;
    case SDL_GL_ACCUM_ALPHA_SIZE:
        attrib = GL_ACCUM_ALPHA_BITS;
        break;
    case SDL_GL_STEREO:
        attrib = GL_STEREO;
        break;
#else
    case SDL_GL_ACCUM_RED_SIZE:
    case SDL_GL_ACCUM_GREEN_SIZE:
    case SDL_GL_ACCUM_BLUE_SIZE:
    case SDL_GL_ACCUM_ALPHA_SIZE:
    case SDL_GL_STEREO:
        /* none of these are supported in OpenGL ES */
        *value = 0;
        return 0;
#endif
    case SDL_GL_MULTISAMPLEBUFFERS:
#ifndef SDL_VIDEO_OPENGL_ES
        attrib = GL_SAMPLE_BUFFERS_ARB;
#else
        attrib = GL_SAMPLE_BUFFERS;
#endif
        break;
    case SDL_GL_MULTISAMPLESAMPLES:
#ifndef SDL_VIDEO_OPENGL_ES
        attrib = GL_SAMPLES_ARB;
#else
        attrib = GL_SAMPLES;
#endif
        break;
    case SDL_GL_BUFFER_SIZE:
        {
            GLint bits = 0;
            GLint component;

            /*
             * there doesn't seem to be a single flag in OpenGL
             * for this!
             */
            glGetIntegervFunc(GL_RED_BITS, &component);
            bits += component;
            glGetIntegervFunc(GL_GREEN_BITS, &component);
            bits += component;
            glGetIntegervFunc(GL_BLUE_BITS, &component);
            bits += component;
            glGetIntegervFunc(GL_ALPHA_BITS, &component);
            bits += component;

            *value = bits;
            return 0;
        }
    case SDL_GL_ACCELERATED_VISUAL:
        {
            /* FIXME: How do we get this information? */
            *value = (_this->gl_config.accelerated != 0);
            return 0;
        }
    default:
        SDL_SetError("Unknown OpenGL attribute");
        return -1;
    }

    glGetIntegervFunc(attrib, (GLint *) value);
    error = glGetErrorFunc();
    if (error != GL_NO_ERROR) {
        switch (error) {
        case GL_INVALID_ENUM:
            {
                SDL_SetError("OpenGL error: GL_INVALID_ENUM");
            }
            break;
        case GL_INVALID_VALUE:
            {
                SDL_SetError("OpenGL error: GL_INVALID_VALUE");
            }
            break;
        default:
            {
                SDL_SetError("OpenGL error: %08X", error);
            }
            break;
        }
        return -1;
    }
    return 0;
#else
    SDL_Unsupported();
    return -1;
#endif /* SDL_VIDEO_OPENGL */
}

SDL_GLContext
SDL_GL_CreateContext(SDL_Window * window)
{
    CHECK_WINDOW_MAGIC(window, NULL);

    if (!(window->flags & SDL_WINDOW_OPENGL)) {
        SDL_SetError("The specified window isn't an OpenGL window");
        return NULL;
    }
    return _this->GL_CreateContext(_this, window);
}

int
SDL_GL_MakeCurrent(SDL_Window * window, SDL_GLContext context)
{
    CHECK_WINDOW_MAGIC(window, -1);

    if (!(window->flags & SDL_WINDOW_OPENGL)) {
        SDL_SetError("The specified window isn't an OpenGL window");
        return -1;
    }
    if (!context) {
        window = NULL;
    }
    return _this->GL_MakeCurrent(_this, window, context);
}

int
SDL_GL_SetSwapInterval(int interval)
{
    if (!_this) {
        SDL_UninitializedVideo();
        return -1;
    }
    if (_this->GL_SetSwapInterval) {
        return _this->GL_SetSwapInterval(_this, interval);
    } else {
        SDL_SetError("Setting the swap interval is not supported");
        return -1;
    }
}

int
SDL_GL_GetSwapInterval(void)
{
    if (!_this) {
        SDL_UninitializedVideo();
        return -1;
    }
    if (_this->GL_GetSwapInterval) {
        return _this->GL_GetSwapInterval(_this);
    } else {
        SDL_SetError("Getting the swap interval is not supported");
        return -1;
    }
}

void
SDL_GL_SwapWindow(SDL_Window * window)
{
    CHECK_WINDOW_MAGIC(window, );

    if (!(window->flags & SDL_WINDOW_OPENGL)) {
        SDL_SetError("The specified window isn't an OpenGL window");
        return;
    }
    _this->GL_SwapWindow(_this, window);
}

void
SDL_GL_DeleteContext(SDL_GLContext context)
{
    if (!_this || !_this->gl_data || !context) {
        return;
    }
    _this->GL_MakeCurrent(_this, NULL, NULL);
    _this->GL_DeleteContext(_this, context);
}

#if 0                           // FIXME
/*
 * Utility function used by SDL_WM_SetIcon(); flags & 1 for color key, flags
 * & 2 for alpha channel.
 */
static void
CreateMaskFromColorKeyOrAlpha(SDL_Surface * icon, Uint8 * mask, int flags)
{
    int x, y;
    Uint32 colorkey;
#define SET_MASKBIT(icon, x, y, mask) \
	mask[(y*((icon->w+7)/8))+(x/8)] &= ~(0x01<<(7-(x%8)))

    colorkey = icon->format->colorkey;
    switch (icon->format->BytesPerPixel) {
    case 1:
        {
            Uint8 *pixels;
            for (y = 0; y < icon->h; ++y) {
                pixels = (Uint8 *) icon->pixels + y * icon->pitch;
                for (x = 0; x < icon->w; ++x) {
                    if (*pixels++ == colorkey) {
                        SET_MASKBIT(icon, x, y, mask);
                    }
                }
            }
        }
        break;

    case 2:
        {
            Uint16 *pixels;
            for (y = 0; y < icon->h; ++y) {
                pixels = (Uint16 *) icon->pixels + y * icon->pitch / 2;
                for (x = 0; x < icon->w; ++x) {
                    if ((flags & 1) && *pixels == colorkey) {
                        SET_MASKBIT(icon, x, y, mask);
                    } else if ((flags & 2)
                               && (*pixels & icon->format->Amask) == 0) {
                        SET_MASKBIT(icon, x, y, mask);
                    }
                    pixels++;
                }
            }
        }
        break;

    case 4:
        {
            Uint32 *pixels;
            for (y = 0; y < icon->h; ++y) {
                pixels = (Uint32 *) icon->pixels + y * icon->pitch / 4;
                for (x = 0; x < icon->w; ++x) {
                    if ((flags & 1) && *pixels == colorkey) {
                        SET_MASKBIT(icon, x, y, mask);
                    } else if ((flags & 2)
                               && (*pixels & icon->format->Amask) == 0) {
                        SET_MASKBIT(icon, x, y, mask);
                    }
                    pixels++;
                }
            }
        }
        break;
    }
}

/*
 * Sets the window manager icon for the display window.
 */
void
SDL_WM_SetIcon(SDL_Surface * icon, Uint8 * mask)
{
    if (icon && _this->SetIcon) {
        /* Generate a mask if necessary, and create the icon! */
        if (mask == NULL) {
            int mask_len = icon->h * (icon->w + 7) / 8;
            int flags = 0;
            mask = (Uint8 *) SDL_malloc(mask_len);
            if (mask == NULL) {
                return;
            }
            SDL_memset(mask, ~0, mask_len);
            if (icon->flags & SDL_SRCCOLORKEY)
                flags |= 1;
            if (icon->flags & SDL_SRCALPHA)
                flags |= 2;
            if (flags) {
                CreateMaskFromColorKeyOrAlpha(icon, mask, flags);
            }
            _this->SetIcon(_this, icon, mask);
            SDL_free(mask);
        } else {
            _this->SetIcon(_this, icon, mask);
        }
    }
}
#endif

SDL_bool
SDL_GetWindowWMInfo(SDL_Window * window, struct SDL_SysWMinfo *info)
{
    CHECK_WINDOW_MAGIC(window, SDL_FALSE);

    if (!info) {
        return SDL_FALSE;
    }
    info->subsystem = SDL_SYSWM_UNKNOWN;

    if (!_this->GetWindowWMInfo) {
        return SDL_FALSE;
    }
    return (_this->GetWindowWMInfo(_this, window, info));
}

void
SDL_StartTextInput(void)
{
    if (_this && _this->StartTextInput) {
        _this->StartTextInput(_this);
    }
    SDL_EventState(SDL_TEXTINPUT, SDL_ENABLE);
    SDL_EventState(SDL_TEXTEDITING, SDL_ENABLE);
}

void
SDL_StopTextInput(void)
{
    if (_this && _this->StopTextInput) {
        _this->StopTextInput(_this);
    }
    SDL_EventState(SDL_TEXTINPUT, SDL_DISABLE);
    SDL_EventState(SDL_TEXTEDITING, SDL_DISABLE);
}

void
SDL_SetTextInputRect(SDL_Rect *rect)
{
    if (_this && _this->SetTextInputRect) {
        _this->SetTextInputRect(_this, rect);
    }
}

/* vi: set ts=4 sw=4 expandtab: */
