/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vita_sdlgl_shim.c - vitaGL implementations behind vita_sdlgl_shim.h. */
#if defined(__vita__)
#include "vita_sdlgl_shim.h"
#include <string.h>

static int s_vglReady = 0;

SDL_GLContext VitaGL_CreateContext(SDL_Window* w)
{
    (void)w;
    if (!s_vglReady)
    {
        /* ram_threshold: RAM vitaGL leaves free for the newlib heap.
         * Tuned later against real memory pressure (map .bss, VRAM tex). */
        vglInitExtended(0, VITA_SCREEN_W, VITA_SCREEN_H,
                        16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
        s_vglReady = 1;
    }
    return (SDL_GLContext)(uintptr_t)1; /* any non-NULL handle */
}

void VitaGL_SwapWindow(SDL_Window* w)
{
    (void)w;
    vglSwapBuffers(GL_FALSE);
}

int VitaGL_SetSwapInterval(int interval)
{
    vglWaitVblankStart(interval ? GL_TRUE : GL_FALSE);
    return 0;
}

void VitaGL_GetDrawableSize(SDL_Window* w, int* x, int* y)
{
    (void)w;
    if (x) *x = VITA_SCREEN_W;
    if (y) *y = VITA_SCREEN_H;
}

int VitaGL_GetAttribute(SDL_GLattr attr, int* value)
{
    if (!value) return -1;
    switch (attr)
    {
    case SDL_GL_RED_SIZE: case SDL_GL_GREEN_SIZE:
    case SDL_GL_BLUE_SIZE: case SDL_GL_ALPHA_SIZE: *value = 8; break;
    case SDL_GL_DEPTH_SIZE:   *value = 32; break;
    case SDL_GL_STENCIL_SIZE: *value = 8; break;
    case SDL_GL_DOUBLEBUFFER: *value = 1; break;
    case SDL_GL_CONTEXT_MAJOR_VERSION: *value = 2; break;
    default: *value = 0; break;
    }
    return 0;
}

void* VitaGL_GetProcAddress(const char* name)
{
    return vglGetProcAddress(name);
}
#endif
