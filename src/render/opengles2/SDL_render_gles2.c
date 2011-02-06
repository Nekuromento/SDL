/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 2010 itsnotabigtruck.

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include "SDL_config.h"

#if SDL_VIDEO_RENDER_OGL_ES2

#include "SDL_opengles2.h"
#include "../SDL_sysrender.h"
#include "SDL_shaders_gles2.h"

/*************************************************************************************************
 * Bootstrap data                                                                                *
 *************************************************************************************************/

static SDL_Renderer *GLES2_CreateRenderer(SDL_Window *window, Uint32 flags);

SDL_RenderDriver GLES2_RenderDriver = {
    GLES2_CreateRenderer,
    {
        "opengles2",
        (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC),
        1,
        {SDL_PIXELFORMAT_ABGR8888},
        0,
        0
    }
};

/*************************************************************************************************
 * Context structures                                                                            *
 *************************************************************************************************/

typedef struct GLES2_TextureData
{
    GLenum texture;
    GLenum texture_type;
    GLenum pixel_format;
    GLenum pixel_type;
    void *pixel_data;
    size_t pitch;
} GLES2_TextureData;

typedef struct GLES2_ShaderCacheEntry
{
    GLuint id;
    GLES2_ShaderType type;
    const GLES2_ShaderInstance *instance;
    int references;
    struct GLES2_ShaderCacheEntry *prev;
    struct GLES2_ShaderCacheEntry *next;
} GLES2_ShaderCacheEntry;

typedef struct GLES2_ShaderCache
{
    int count;
    GLES2_ShaderCacheEntry *head;
} GLES2_ShaderCache;

typedef struct GLES2_ProgramCacheEntry
{
    GLuint id;
    SDL_BlendMode blend_mode;
    GLES2_ShaderCacheEntry *vertex_shader;
    GLES2_ShaderCacheEntry *fragment_shader;
    GLuint uniform_locations[16];
    struct GLES2_ProgramCacheEntry *prev;
    struct GLES2_ProgramCacheEntry *next;
} GLES2_ProgramCacheEntry;

typedef struct GLES2_ProgramCache
{
    int count;
    GLES2_ProgramCacheEntry *head;
    GLES2_ProgramCacheEntry *tail;
} GLES2_ProgramCache;

typedef enum
{
    GLES2_ATTRIBUTE_POSITION = 0,
    GLES2_ATTRIBUTE_TEXCOORD = 1
} GLES2_Attribute;

typedef enum
{
    GLES2_UNIFORM_PROJECTION,
    GLES2_UNIFORM_TEXTURE,
    GLES2_UNIFORM_MODULATION,
    GLES2_UNIFORM_COLOR,
    GLES2_UNIFORM_COLORTABLE
} GLES2_Uniform;

typedef enum
{
    GLES2_IMAGESOURCE_SOLID,
    GLES2_IMAGESOURCE_TEXTURE
} GLES2_ImageSource;

typedef struct GLES2_DriverContext
{
    SDL_GLContext *context;
    int shader_format_count;
    GLenum *shader_formats;
    GLES2_ShaderCache shader_cache;
    GLES2_ProgramCache program_cache;
    GLES2_ProgramCacheEntry *current_program;
	SDL_bool updateSize;
} GLES2_DriverContext;

#define GLES2_MAX_CACHED_PROGRAMS 8

/*************************************************************************************************
 * Renderer state APIs                                                                           *
 *************************************************************************************************/

static void GLES2_WindowEvent(SDL_Renderer * renderer,
                              const SDL_WindowEvent *event);
static int GLES2_ActivateRenderer(SDL_Renderer *renderer);
static int GLES2_DisplayModeChanged(SDL_Renderer *renderer);
static void GLES2_DestroyRenderer(SDL_Renderer *renderer);

static SDL_GLContext SDL_CurrentContext = NULL;

static int
GLES2_ActivateRenderer(SDL_Renderer * renderer)
{
    GLES2_DriverContext *rdata = (GLES2_DriverContext *)renderer->driverdata;
    SDL_Window *window = renderer->window;

    if (SDL_CurrentContext != rdata->context) {
        /* Null out the current program to ensure we set it again */
        rdata->current_program = NULL;

        if (SDL_GL_MakeCurrent(window, rdata->context) < 0) {
            return -1;
        }
        SDL_CurrentContext = rdata->context;
    }
    if (rdata->updateSize) {
        int w, h;

        SDL_GetWindowSize(window, &w, &h);
        glViewport(0, 0, w, h);
        rdata->updateSize = SDL_FALSE;
    }
    return 0;
}

static void
GLES2_WindowEvent(SDL_Renderer * renderer, const SDL_WindowEvent *event)
{
    GLES2_DriverContext *rdata = (GLES2_DriverContext *)renderer->driverdata;

    if (event->event == SDL_WINDOWEVENT_RESIZED) {
        /* Rebind the context to the window area */
        SDL_CurrentContext = NULL;
        rdata->updateSize = SDL_TRUE;
    }
}

static void
GLES2_DestroyRenderer(SDL_Renderer *renderer)
{
    GLES2_DriverContext *rdata = (GLES2_DriverContext *)renderer->driverdata;
    GLES2_ProgramCacheEntry *entry;
    GLES2_ProgramCacheEntry *next;

    GLES2_ActivateRenderer(renderer);

    /* Deallocate everything */
    entry = rdata->program_cache.head;
    while (entry)
    {
        glDeleteShader(entry->vertex_shader->id);
        glDeleteShader(entry->fragment_shader->id);
        SDL_free(entry->vertex_shader);
        SDL_free(entry->fragment_shader);
        glDeleteProgram(entry->id);
        next = entry->next;
        SDL_free(entry);
        entry = next;
    }
    SDL_GL_DeleteContext(rdata->context);
    SDL_free(rdata->shader_formats);
    SDL_free(renderer->driverdata);
    SDL_free(renderer);
}

/*************************************************************************************************
 * Texture APIs                                                                                  *
 *************************************************************************************************/

static int GLES2_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture);
static void GLES2_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture);
static int GLES2_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect,
                             void **pixels, int *pitch);
static void GLES2_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture);
static int GLES2_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect,
                               const void *pixels, int pitch);

static int
GLES2_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    GLES2_DriverContext *rdata = (GLES2_DriverContext *)renderer->driverdata;
    GLES2_TextureData *tdata;
    GLenum format;
    GLenum type;

    GLES2_ActivateRenderer(renderer);

    /* Determine the corresponding GLES texture format params */
    switch (texture->format)
    {
    case SDL_PIXELFORMAT_ABGR8888:
        format = GL_RGBA;
        type = GL_UNSIGNED_BYTE;
        break;
    default:
        SDL_SetError("Texture format not supported");
        return -1;
    }

    /* Allocate a texture struct */
    tdata = (GLES2_TextureData *)SDL_calloc(1, sizeof(GLES2_TextureData));
    if (!tdata)
    {
        SDL_OutOfMemory();
        return -1;
    }
    tdata->texture = 0;
    tdata->texture_type = GL_TEXTURE_2D;
    tdata->pixel_format = format;
    tdata->pixel_type = type;

    /* Allocate a blob for image data */
    if (texture->access == SDL_TEXTUREACCESS_STREAMING)
    {
        tdata->pitch = texture->w * SDL_BYTESPERPIXEL(texture->format);
        tdata->pixel_data = SDL_malloc(tdata->pitch * texture->h);
        if (!tdata->pixel_data)
        {
            SDL_OutOfMemory();
            SDL_free(tdata);
            return -1;
        }
    }

    /* Allocate the texture */
    glGetError();
    glGenTextures(1, &tdata->texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(tdata->texture_type, tdata->texture);
    glTexParameteri(tdata->texture_type, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(tdata->texture_type, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(tdata->texture_type, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(tdata->texture_type, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(tdata->texture_type, 0, format, texture->w, texture->h, 0, format, type, NULL);
    if (glGetError() != GL_NO_ERROR)
    {
        SDL_SetError("Texture creation failed");
        glDeleteTextures(1, &tdata->texture);
        SDL_free(tdata);
        return -1;
    }
    texture->driverdata = tdata;
    return 0;
}

static void
GLES2_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    GLES2_TextureData *tdata = (GLES2_TextureData *)texture->driverdata;

    GLES2_ActivateRenderer(renderer);

    /* Destroy the texture */
    if (tdata)
    {
        glDeleteTextures(1, &tdata->texture);
        SDL_free(tdata->pixel_data);
        SDL_free(tdata);
        texture->driverdata = NULL;
    }
}

static int
GLES2_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect,
                  void **pixels, int *pitch)
{
    GLES2_TextureData *tdata = (GLES2_TextureData *)texture->driverdata;

    /* Retrieve the buffer/pitch for the specified region */
    *pixels = (Uint8 *)tdata->pixel_data +
              (tdata->pitch * rect->y) +
              (rect->x * SDL_BYTESPERPIXEL(texture->format));
    *pitch = tdata->pitch;

    return 0;
}

static void
GLES2_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    GLES2_TextureData *tdata = (GLES2_TextureData *)texture->driverdata;

    GLES2_ActivateRenderer(renderer);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(tdata->texture_type, tdata->texture);
    glTexSubImage2D(tdata->texture_type, 0, 0, 0, texture->w, texture->h,
                    tdata->pixel_format, tdata->pixel_type, tdata->pixel_data);
}

static int
GLES2_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect,
                    const void *pixels, int pitch)
{
    GLES2_TextureData *tdata = (GLES2_TextureData *)texture->driverdata;
    Uint8 *blob = NULL;
    Uint8 *src;
    int srcPitch;
    Uint8 *dest;
    int y;

    GLES2_ActivateRenderer(renderer);

    /* Bail out if we're supposed to update an empty rectangle */
    if (rect->w <= 0 || rect->h <= 0)
        return 0;

    /* Reformat the texture data into a tightly packed array */
    srcPitch = rect->w * SDL_BYTESPERPIXEL(texture->format);
    src = (Uint8 *)pixels;
    if (pitch != srcPitch)
    {
        blob = (Uint8 *)SDL_malloc(srcPitch * rect->h);
        if (!blob)
        {
            SDL_OutOfMemory();
            return -1;
        }
        src = blob;
        for (y = 0; y < rect->h; ++y)
        {
            SDL_memcpy(src, pixels, srcPitch);
            src += srcPitch;
            pixels = (Uint8 *)pixels + pitch;
        }
        src = blob;
    }

    /* Create a texture subimage with the supplied data */
    glGetError();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(tdata->texture_type, tdata->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(tdata->texture_type,
                    0,
                    rect->x,
                    rect->y,
                    rect->w,
                    rect->h,
                    tdata->pixel_format,
                    tdata->pixel_type,
                    src);
    if (glGetError() != GL_NO_ERROR)
    {
        SDL_SetError("Failed to update texture");
        return -1;
    }

    /* Update the (streaming) texture buffer, in one pass if possible */
    if (tdata->pixel_data)
    {
        dest = (Uint8 *)tdata->pixel_data +
               (tdata->pitch * rect->y) +
               (SDL_BYTESPERPIXEL(texture->format) * rect->x);
        if (rect->w == texture->w)
        {
            SDL_memcpy(dest, src, srcPitch * rect->h);
        }
        else
        {
            for (y = 0; y < rect->h; ++y)
            {
                SDL_memcpy(dest, src, srcPitch);
                src += srcPitch;
                dest += tdata->pitch;
            }
        }
    }

    /* Clean up and return */
    SDL_free(blob);
    return 0;
}

/*************************************************************************************************
 * Shader management functions                                                                   *
 *************************************************************************************************/

static GLES2_ShaderCacheEntry *GLES2_CacheShader(SDL_Renderer *renderer, GLES2_ShaderType type,
                                                 SDL_BlendMode blendMode);
static void GLES2_EvictShader(SDL_Renderer *renderer, GLES2_ShaderCacheEntry *entry);
static GLES2_ProgramCacheEntry *GLES2_CacheProgram(SDL_Renderer *renderer,
                                                   GLES2_ShaderCacheEntry *vertex,
                                                   GLES2_ShaderCacheEntry *fragment,
                                                   SDL_BlendMode blendMode);
static int GLES2_SelectProgram(SDL_Renderer *renderer, GLES2_ImageSource source,
                               SDL_BlendMode blendMode);
static int GLES2_SetOrthographicProjection(SDL_Renderer *renderer);

static GLES2_ProgramCacheEntry *
GLES2_CacheProgram(SDL_Renderer *renderer, GLES2_ShaderCacheEntry *vertex,
                   GLES2_ShaderCacheEntry *fragment, SDL_BlendMode blendMode)
{
    GLES2_DriverContext *rdata = (GLES2_DriverContext *)renderer->driverdata;
    GLES2_ProgramCacheEntry *entry;
    GLES2_ShaderCacheEntry *shaderEntry;
    GLint linkSuccessful;

    /* Check if we've already cached this program */
    entry = rdata->program_cache.head;
    while (entry)
    {
        if (entry->vertex_shader == vertex && entry->fragment_shader == fragment)
            break;
        entry = entry->next;
    }
    if (entry)
    {
        if (rdata->program_cache.count > 1)
        {
            if (entry->next)
                entry->next->prev = entry->prev;
            if (entry->prev)
                entry->prev->next = entry->next;
            entry->prev = NULL;
            entry->next = rdata->program_cache.head;
            rdata->program_cache.head->prev = entry;
            rdata->program_cache.head = entry;
        }
        return entry;
    }

    /* Create a program cache entry */
    entry = (GLES2_ProgramCacheEntry *)SDL_calloc(1, sizeof(GLES2_ProgramCacheEntry));
    if (!entry)
    {
        SDL_OutOfMemory();
        return NULL;
    }
    entry->vertex_shader = vertex;
    entry->fragment_shader = fragment;
    entry->blend_mode = blendMode;
    
    /* Create the program and link it */
    glGetError();
    entry->id = glCreateProgram();
    glAttachShader(entry->id, vertex->id);
    glAttachShader(entry->id, fragment->id);
    glBindAttribLocation(entry->id, GLES2_ATTRIBUTE_POSITION, "a_position");
    glBindAttribLocation(entry->id, GLES2_ATTRIBUTE_TEXCOORD, "a_texCoord");
    glLinkProgram(entry->id);
    glGetProgramiv(entry->id, GL_LINK_STATUS, &linkSuccessful);
    if (glGetError() != GL_NO_ERROR || !linkSuccessful)
    {
        SDL_SetError("Failed to link shader program");
        glDeleteProgram(entry->id);
        SDL_free(entry);
        return NULL;
    }
    
    /* Predetermine locations of uniform variables */
    entry->uniform_locations[GLES2_UNIFORM_PROJECTION] =
        glGetUniformLocation(entry->id, "u_projection");
    entry->uniform_locations[GLES2_UNIFORM_TEXTURE] =
        glGetUniformLocation(entry->id, "u_texture");
    entry->uniform_locations[GLES2_UNIFORM_MODULATION] =
        glGetUniformLocation(entry->id, "u_modulation");
    entry->uniform_locations[GLES2_UNIFORM_COLOR] =
        glGetUniformLocation(entry->id, "u_color");
    entry->uniform_locations[GLES2_UNIFORM_COLORTABLE] =
        glGetUniformLocation(entry->id, "u_colorTable");

    /* Cache the linked program */
    if (rdata->program_cache.head)
    {
        entry->next = rdata->program_cache.head;
        rdata->program_cache.head->prev = entry;
    }
    else
    {
        rdata->program_cache.tail = entry;
    }
    rdata->program_cache.head = entry;
    ++rdata->program_cache.count;

    /* Increment the refcount of the shaders we're using */
    ++vertex->references;
    ++fragment->references;

    /* Evict the last entry from the cache if we exceed the limit */
    if (rdata->program_cache.count > GLES2_MAX_CACHED_PROGRAMS)
    {
        shaderEntry = rdata->program_cache.tail->vertex_shader;
        if (--shaderEntry->references <= 0)
            GLES2_EvictShader(renderer, shaderEntry);
        shaderEntry = rdata->program_cache.tail->fragment_shader;
        if (--shaderEntry->references <= 0)
            GLES2_EvictShader(renderer, shaderEntry);
        glDeleteProgram(rdata->program_cache.tail->id);
        rdata->program_cache.tail = rdata->program_cache.tail->prev;
        SDL_free(rdata->program_cache.tail->next);
        rdata->program_cache.tail->next = NULL;
        --rdata->program_cache.count;
    }
    return entry;
}

static GLES2_ShaderCacheEntry *
GLES2_CacheShader(SDL_Renderer *renderer, GLES2_ShaderType type, SDL_BlendMode blendMode)
{
    GLES2_DriverContext *rdata = (GLES2_DriverContext *)renderer->driverdata;
    const GLES2_Shader *shader;
    const GLES2_ShaderInstance *instance = NULL;
    GLES2_ShaderCacheEntry *entry = NULL;
    GLint compileSuccessful = GL_FALSE;
    int i, j;

    /* Find the corresponding shader */
    shader = GLES2_GetShader(type, blendMode);
    if (!shader)
    {
        SDL_SetError("No shader matching the requested characteristics was found");
        return NULL;
    }
    
    /* Find a matching shader instance that's supported on this hardware */
    for (i = 0; i < shader->instance_count; ++i)
    {
        for (j = 0; j < rdata->shader_format_count; ++j)
        {
            if (!shader->instances)
                continue;
            if (shader->instances[i]->format != rdata->shader_formats[j])
                continue;
            instance = shader->instances[i];
            break;
        }
    }
    if (!instance)
    {
        SDL_SetError("The specified shader cannot be loaded on the current platform");
        return NULL;
    }

    /* Check if we've already cached this shader */
    entry = rdata->shader_cache.head;
    while (entry)
    {
        if (entry->instance == instance)
            break;
        entry = entry->next;
    }
    if (entry)
        return entry;

    /* Create a shader cache entry */
    entry = (GLES2_ShaderCacheEntry *)SDL_calloc(1, sizeof(GLES2_ShaderCacheEntry));
    if (!entry)
    {
        SDL_OutOfMemory();
        return NULL;
    }
    entry->type = type;
    entry->instance = instance;

    /* Compile or load the selected shader instance */
    glGetError();
    entry->id = glCreateShader(instance->type);
    if (instance->format == (GLenum)-1)
    {
        glShaderSource(entry->id, 1, (const char **)&instance->data, &instance->length);
        glCompileShader(entry->id);
        glGetShaderiv(entry->id, GL_COMPILE_STATUS, &compileSuccessful);
    }
    else
    {
        glShaderBinary(1, &entry->id, instance->format, instance->data, instance->length);
        compileSuccessful = GL_TRUE;
    }
    if (glGetError() != GL_NO_ERROR || !compileSuccessful)
    {
        SDL_SetError("Failed to load the specified shader");
        glDeleteShader(entry->id);
        SDL_free(entry);
        return NULL;
    }

    /* Link the shader entry in at the front of the cache */
    if (rdata->shader_cache.head)
    {
        entry->next = rdata->shader_cache.head;
        rdata->shader_cache.head->prev = entry;
    }
    rdata->shader_cache.head = entry;
    ++rdata->shader_cache.count;
    return entry;
}

static void
GLES2_EvictShader(SDL_Renderer *renderer, GLES2_ShaderCacheEntry *entry)
{
    GLES2_DriverContext *rdata = (GLES2_DriverContext *)renderer->driverdata;

    /* Unlink the shader from the cache */
    if (entry->next)
        entry->next->prev = entry->prev;
    if (entry->prev)
        entry->prev->next = entry->next;
    if (rdata->shader_cache.head == entry)
        rdata->shader_cache.head = entry->next;
    --rdata->shader_cache.count;

    /* Deallocate the shader */
    glDeleteShader(entry->id);
    SDL_free(entry);
}

static int
GLES2_SelectProgram(SDL_Renderer *renderer, GLES2_ImageSource source, SDL_BlendMode blendMode)
{
    GLES2_DriverContext *rdata = (GLES2_DriverContext *)renderer->driverdata;
    GLES2_ShaderCacheEntry *vertex = NULL;
    GLES2_ShaderCacheEntry *fragment = NULL;
    GLES2_ShaderType vtype, ftype;
    GLES2_ProgramCacheEntry *program;

    /* Select an appropriate shader pair for the specified modes */
    vtype = GLES2_SHADER_VERTEX_DEFAULT;
    switch (source)
    {
    case GLES2_IMAGESOURCE_SOLID:
        ftype = GLES2_SHADER_FRAGMENT_SOLID_SRC;
        break;
    case GLES2_IMAGESOURCE_TEXTURE:
        ftype = GLES2_SHADER_FRAGMENT_TEXTURE_SRC;
        break;
    }

    /* Load the requested shaders */
    vertex = GLES2_CacheShader(renderer, vtype, blendMode);
    if (!vertex)
        goto fault;
    fragment = GLES2_CacheShader(renderer, ftype, blendMode);
    if (!fragment)
        goto fault;

    /* Check if we need to change programs at all */
    if (rdata->current_program &&
        rdata->current_program->vertex_shader == vertex &&
        rdata->current_program->fragment_shader == fragment)
        return 0;

    /* Generate a matching program */
    program = GLES2_CacheProgram(renderer, vertex, fragment, blendMode);
    if (!program)
        goto fault;

    /* Select that program in OpenGL */
    glGetError();
    glUseProgram(program->id);
    if (glGetError() != GL_NO_ERROR)
    {
        SDL_SetError("Failed to select program");
        goto fault;
    }

    /* Set the current program */
    rdata->current_program = program;

    /* Activate an orthographic projection */
    if (GLES2_SetOrthographicProjection(renderer) < 0)
        goto fault;

    /* Clean up and return */
    return 0;
fault:
    if (vertex && vertex->references <= 0)
        GLES2_EvictShader(renderer, vertex);
    if (fragment && fragment->references <= 0)
        GLES2_EvictShader(renderer, fragment);
    rdata->current_program = NULL;
    return -1;
}

static int
GLES2_SetOrthographicProjection(SDL_Renderer *renderer)
{
    GLES2_DriverContext *rdata = (GLES2_DriverContext *)renderer->driverdata;
    SDL_Window *window = renderer->window;
    int w, h;
    GLfloat projection[4][4];
    GLuint locProjection;

    /* Get the window width and height */
    SDL_GetWindowSize(window, &w, &h);

    /* Prepare an orthographic projection */
    projection[0][0] = 2.0f / w;
    projection[0][1] = 0.0f;
    projection[0][2] = 0.0f;
    projection[0][3] = 0.0f;
    projection[1][0] = 0.0f;
    projection[1][1] = -2.0f / h;
    projection[1][2] = 0.0f;
    projection[1][3] = 0.0f;
    projection[2][0] = 0.0f;
    projection[2][1] = 0.0f;
    projection[2][2] = 1.0f;
    projection[2][3] = 0.0f;
    projection[3][0] = -1.0f;
    projection[3][1] = 1.0f;
    projection[3][2] = 0.0f;
    projection[3][3] = 1.0f;

    /* Set the projection matrix */
    locProjection = rdata->current_program->uniform_locations[GLES2_UNIFORM_PROJECTION];
    glGetError();
    glUniformMatrix4fv(locProjection, 1, GL_FALSE, (GLfloat *)projection);
    if (glGetError() != GL_NO_ERROR)
    {
        SDL_SetError("Failed to set orthographic projection");
        return -1;
    }
    return 0;
}

/*************************************************************************************************
 * Rendering functions                                                                           *
 *************************************************************************************************/

static int GLES2_RenderClear(SDL_Renderer *renderer);
static int GLES2_RenderDrawPoints(SDL_Renderer *renderer, const SDL_Point *points, int count);
static int GLES2_RenderDrawLines(SDL_Renderer *renderer, const SDL_Point *points, int count);
static int GLES2_RenderDrawRects(SDL_Renderer *renderer, const SDL_Rect **rects, int count);
static int GLES2_RenderFillRects(SDL_Renderer *renderer, const SDL_Rect **rects, int count);
static int GLES2_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect,
                            const SDL_Rect *dstrect);
static void GLES2_RenderPresent(SDL_Renderer *renderer);

static int
GLES2_RenderClear(SDL_Renderer *renderer)
{
    float r = (float)renderer->r / 255.0f;
    float g = (float)renderer->g / 255.0f;
    float b = (float)renderer->b / 255.0f;
    float a = (float)renderer->a / 255.0f;

    GLES2_ActivateRenderer(renderer);

    /* Clear the backbuffer with the selected color */
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    return 0;
}

static void
GLES2_SetBlendMode(int blendMode)
{
    switch (blendMode)
    {
    case SDL_BLENDMODE_NONE:
    default:
        glDisable(GL_BLEND);
        break;
    case SDL_BLENDMODE_BLEND:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case SDL_BLENDMODE_ADD:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
    case SDL_BLENDMODE_MOD:
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        break;
    }
}

static int
GLES2_RenderDrawPoints(SDL_Renderer *renderer, const SDL_Point *points, int count)
{
    GLES2_DriverContext *rdata = (GLES2_DriverContext *)renderer->driverdata;
    GLfloat *vertices;
    SDL_BlendMode blendMode;
    int alpha;
    GLuint locColor;
    int idx;

    GLES2_ActivateRenderer(renderer);

    blendMode = renderer->blendMode;
    alpha = renderer->a;

    /* Activate an appropriate shader and set the projection matrix */
    if (GLES2_SelectProgram(renderer, GLES2_IMAGESOURCE_SOLID, blendMode) < 0)
        return -1;

    /* Select the color to draw with */
    locColor = rdata->current_program->uniform_locations[GLES2_UNIFORM_COLOR];
    glGetError();
    glUniform4f(locColor,
                renderer->r / 255.0f,
                renderer->g / 255.0f,
                renderer->b / 255.0f,
                alpha / 255.0f);

    /* Configure the correct blend mode */
    GLES2_SetBlendMode(blendMode);

    /* Emit the specified vertices as points */
    vertices = SDL_stack_alloc(GLfloat, count * 2);
    for (idx = 0; idx < count; ++idx)
    {
        GLfloat x = (GLfloat)points[idx].x + 0.5f;
        GLfloat y = (GLfloat)points[idx].y + 0.5f;

        vertices[idx * 2] = x;
        vertices[(idx * 2) + 1] = y;
    }
    glEnableVertexAttribArray(GLES2_ATTRIBUTE_POSITION);
    glVertexAttribPointer(GLES2_ATTRIBUTE_POSITION, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glDrawArrays(GL_POINTS, 0, count);
    glDisableVertexAttribArray(GLES2_ATTRIBUTE_POSITION);
    SDL_stack_free(vertices);
    if (glGetError() != GL_NO_ERROR)
    {
        SDL_SetError("Failed to render lines");
        return -1;
    }
    return 0;
}

static int
GLES2_RenderDrawLines(SDL_Renderer *renderer, const SDL_Point *points, int count)
{
    GLES2_DriverContext *rdata = (GLES2_DriverContext *)renderer->driverdata;
    GLfloat *vertices;
    SDL_BlendMode blendMode;
    int alpha;
    GLuint locColor;
    int idx;

    GLES2_ActivateRenderer(renderer);

    blendMode = renderer->blendMode;
    alpha = renderer->a;

    /* Activate an appropriate shader and set the projection matrix */
    if (GLES2_SelectProgram(renderer, GLES2_IMAGESOURCE_SOLID, blendMode) < 0)
        return -1;

    /* Select the color to draw with */
    locColor = rdata->current_program->uniform_locations[GLES2_UNIFORM_COLOR];
    glGetError();
    glUniform4f(locColor,
                renderer->r / 255.0f,
                renderer->g / 255.0f,
                renderer->b / 255.0f,
                alpha / 255.0f);

    /* Configure the correct blend mode */
    GLES2_SetBlendMode(blendMode);

    /* Emit a line strip including the specified vertices */
    vertices = SDL_stack_alloc(GLfloat, count * 2);
    for (idx = 0; idx < count; ++idx)
    {
        GLfloat x = (GLfloat)points[idx].x + 0.5f;
        GLfloat y = (GLfloat)points[idx].y + 0.5f;

        vertices[idx * 2] = x;
        vertices[(idx * 2) + 1] = y;
    }
    glEnableVertexAttribArray(GLES2_ATTRIBUTE_POSITION);
    glVertexAttribPointer(GLES2_ATTRIBUTE_POSITION, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glDrawArrays(GL_LINE_STRIP, 0, count);
    glDisableVertexAttribArray(GLES2_ATTRIBUTE_POSITION);
    SDL_stack_free(vertices);
    if (glGetError() != GL_NO_ERROR)
    {
        SDL_SetError("Failed to render lines");
        return -1;
    }
    return 0;
}

static int
GLES2_RenderFillRects(SDL_Renderer *renderer, const SDL_Rect **rects, int count)
{
    GLES2_DriverContext *rdata = (GLES2_DriverContext *)renderer->driverdata;
    GLfloat vertices[8];
    SDL_BlendMode blendMode;
    int alpha;
    GLuint locColor;
    int idx;

    GLES2_ActivateRenderer(renderer);

    blendMode = renderer->blendMode;
    alpha = renderer->a;

    /* Activate an appropriate shader and set the projection matrix */
    if (GLES2_SelectProgram(renderer, GLES2_IMAGESOURCE_SOLID, blendMode) < 0)
        return -1;

    /* Select the color to draw with */
    locColor = rdata->current_program->uniform_locations[GLES2_UNIFORM_COLOR];
    glGetError();
    glUniform4f(locColor,
                renderer->r / 255.0f,
                renderer->g / 255.0f,
                renderer->b / 255.0f,
                alpha / 255.0f);

    /* Configure the correct blend mode */
    GLES2_SetBlendMode(blendMode);

    /* Emit a line loop for each rectangle */
    glEnableVertexAttribArray(GLES2_ATTRIBUTE_POSITION);
    for (idx = 0; idx < count; ++idx)
    {
        GLfloat xMin = (GLfloat)rects[idx]->x;
        GLfloat xMax = (GLfloat)(rects[idx]->x + rects[idx]->w);
        GLfloat yMin = (GLfloat)rects[idx]->y;
        GLfloat yMax = (GLfloat)(rects[idx]->y + rects[idx]->h);

        vertices[0] = xMin;
        vertices[1] = yMin;
        vertices[2] = xMax;
        vertices[3] = yMin;
        vertices[4] = xMin;
        vertices[5] = yMax;
        vertices[6] = xMax;
        vertices[7] = yMax;
        glVertexAttribPointer(GLES2_ATTRIBUTE_POSITION, 2, GL_FLOAT, GL_FALSE, 0, vertices);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisableVertexAttribArray(GLES2_ATTRIBUTE_POSITION);
    if (glGetError() != GL_NO_ERROR)
    {
        SDL_SetError("Failed to render lines");
        return -1;
    }
    return 0;
}

static int
GLES2_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect,
                 const SDL_Rect *dstrect)
{
    GLES2_DriverContext *rdata = (GLES2_DriverContext *)renderer->driverdata;
    GLES2_TextureData *tdata = (GLES2_TextureData *)texture->driverdata;
    GLES2_ImageSource sourceType;
    SDL_BlendMode blendMode;
    int alpha;
    GLfloat vertices[8];
    GLfloat texCoords[8];
    GLuint locTexture;
    GLuint locModulation;
    GLuint locColorTable;

    GLES2_ActivateRenderer(renderer);

    /* Activate an appropriate shader and set the projection matrix */
    blendMode = texture->blendMode;
    alpha = texture->a;
    sourceType = GLES2_IMAGESOURCE_TEXTURE;
    if (GLES2_SelectProgram(renderer, sourceType, blendMode) < 0)
        return -1;

    /* Select the target texture */
    locTexture = rdata->current_program->uniform_locations[GLES2_UNIFORM_TEXTURE];
    glGetError();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(tdata->texture_type, tdata->texture);
    glUniform1i(locTexture, 0);

    /* Configure texture blending */
    GLES2_SetBlendMode(blendMode);

    /* Configure color modulation */
    locModulation = rdata->current_program->uniform_locations[GLES2_UNIFORM_MODULATION];
    glUniform4f(locModulation,
                texture->r / 255.0f,
                texture->g / 255.0f,
                texture->b / 255.0f,
                alpha / 255.0f);

    /* Emit the textured quad */
    glEnableVertexAttribArray(GLES2_ATTRIBUTE_TEXCOORD);
    glEnableVertexAttribArray(GLES2_ATTRIBUTE_POSITION);
    vertices[0] = (GLfloat)dstrect->x;
    vertices[1] = (GLfloat)dstrect->y;
    vertices[2] = (GLfloat)(dstrect->x + dstrect->w);
    vertices[3] = (GLfloat)dstrect->y;
    vertices[4] = (GLfloat)dstrect->x;
    vertices[5] = (GLfloat)(dstrect->y + dstrect->h);
    vertices[6] = (GLfloat)(dstrect->x + dstrect->w);
    vertices[7] = (GLfloat)(dstrect->y + dstrect->h);
    glVertexAttribPointer(GLES2_ATTRIBUTE_POSITION, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    texCoords[0] = srcrect->x / (GLfloat)texture->w;
    texCoords[1] = srcrect->y / (GLfloat)texture->h;
    texCoords[2] = (srcrect->x + srcrect->w) / (GLfloat)texture->w;
    texCoords[3] = srcrect->y / (GLfloat)texture->h;
    texCoords[4] = srcrect->x / (GLfloat)texture->w;
    texCoords[5] = (srcrect->y + srcrect->h) / (GLfloat)texture->h;
    texCoords[6] = (srcrect->x + srcrect->w) / (GLfloat)texture->w;
    texCoords[7] = (srcrect->y + srcrect->h) / (GLfloat)texture->h;
    glVertexAttribPointer(GLES2_ATTRIBUTE_TEXCOORD, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(GLES2_ATTRIBUTE_POSITION);
    glDisableVertexAttribArray(GLES2_ATTRIBUTE_TEXCOORD);
    if (glGetError() != GL_NO_ERROR)
    {
        SDL_SetError("Failed to render texture");
        return -1;
    }
    return 0;
}

static void
GLES2_RenderPresent(SDL_Renderer *renderer)
{
    GLES2_ActivateRenderer(renderer);

    /* Tell the video driver to swap buffers */
    SDL_GL_SwapWindow(renderer->window);
}

/*************************************************************************************************
 * Renderer instantiation                                                                        *
 *************************************************************************************************/

#define GL_NVIDIA_PLATFORM_BINARY_NV 0x890B

static SDL_Renderer *
GLES2_CreateRenderer(SDL_Window *window, Uint32 flags)
{
    SDL_Renderer *renderer;
    GLES2_DriverContext *rdata;
    GLint nFormats;
#ifndef ZUNE_HD
    GLboolean hasCompiler;
#endif

    /* Create the renderer struct */
    renderer = (SDL_Renderer *)SDL_calloc(1, sizeof(SDL_Renderer));
    rdata = (GLES2_DriverContext *)SDL_calloc(1, sizeof(GLES2_DriverContext));
    if (!renderer)
    {
        SDL_OutOfMemory();
        SDL_free(renderer);
        SDL_free(rdata);
        return NULL;
    }
    renderer->info = GLES2_RenderDriver.info;
    renderer->window = window;
    renderer->driverdata = rdata;

    renderer->info.flags = SDL_RENDERER_ACCELERATED;

    /* Create the GL context */
    rdata->context = SDL_GL_CreateContext(window);
    if (!rdata->context)
    {
        SDL_free(renderer);
        SDL_free(rdata);
        return NULL;
    }
    if (SDL_GL_MakeCurrent(window, rdata->context) < 0) {
        SDL_free(renderer);
        SDL_free(rdata);
        return NULL;
    }

    if (flags & SDL_RENDERER_PRESENTVSYNC) {
        SDL_GL_SetSwapInterval(1);
    } else {
        SDL_GL_SetSwapInterval(0);
    }
    if (SDL_GL_GetSwapInterval() > 0) {
        renderer->info.flags |= SDL_RENDERER_PRESENTVSYNC;
    }

    /* Determine supported shader formats */
    /* HACK: glGetInteger is broken on the Zune HD's compositor, so we just hardcode this */
    glGetError();
#ifdef ZUNE_HD
    nFormats = 1;
#else /* !ZUNE_HD */
    glGetIntegerv(GL_NUM_SHADER_BINARY_FORMATS, &nFormats);
    glGetBooleanv(GL_SHADER_COMPILER, &hasCompiler);
    if (hasCompiler)
        ++nFormats;
#endif /* ZUNE_HD */
    rdata->shader_formats = (GLenum *)SDL_calloc(nFormats, sizeof(GLenum));
    if (!rdata->shader_formats)
    {
        SDL_OutOfMemory();
        SDL_free(renderer);
        SDL_free(rdata);
        return NULL;
    }
    rdata->shader_format_count = nFormats;
#ifdef ZUNE_HD
    rdata->shader_formats[0] = GL_NVIDIA_PLATFORM_BINARY_NV;
#else /* !ZUNE_HD */
    glGetIntegerv(GL_SHADER_BINARY_FORMATS, (GLint *)rdata->shader_formats);
    if (glGetError() != GL_NO_ERROR)
    {
        SDL_SetError("Failed to query supported shader formats");
        SDL_free(renderer);
        SDL_free(rdata->shader_formats);
        SDL_free(rdata);
        return NULL;
    }
    if (hasCompiler)
        rdata->shader_formats[nFormats - 1] = (GLenum)-1;
#endif /* ZUNE_HD */

    /* Populate the function pointers for the module */
    renderer->WindowEvent         = &GLES2_WindowEvent;
    renderer->CreateTexture       = &GLES2_CreateTexture;
    renderer->UpdateTexture       = &GLES2_UpdateTexture;
    renderer->LockTexture         = &GLES2_LockTexture;
    renderer->UnlockTexture       = &GLES2_UnlockTexture;
    renderer->RenderClear         = &GLES2_RenderClear;
    renderer->RenderDrawPoints    = &GLES2_RenderDrawPoints;
    renderer->RenderDrawLines     = &GLES2_RenderDrawLines;
    renderer->RenderFillRects     = &GLES2_RenderFillRects;
    renderer->RenderCopy          = &GLES2_RenderCopy;
    renderer->RenderPresent       = &GLES2_RenderPresent;
    renderer->DestroyTexture      = &GLES2_DestroyTexture;
    renderer->DestroyRenderer     = &GLES2_DestroyRenderer;
    return renderer;
}

#endif /* SDL_VIDEO_RENDER_OGL_ES2 */

/* vi: set ts=4 sw=4 expandtab: */
