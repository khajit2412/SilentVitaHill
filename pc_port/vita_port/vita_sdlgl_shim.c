/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vita_sdlgl_shim.c - vitaGL implementations behind vita_sdlgl_shim.h. */
#if defined(__vita__)
#include "vita_sdlgl_shim.h"
#include <stdio.h>
#include <string.h>
#include <vitashark.h>

extern FILE* g_ShDebugLog;

/* vitaGL compiles GLSL inside glLinkProgram (VGL_MODE_POSTPONED) and crashes on
 * the NULL program a failed compile leaves, before any GL error reaches the
 * caller. vitaShaRK's own log is the only place the reason is reported. */
static void VitaGL_SharkLog(const char* msg, shark_log_level level, int line)
{
    static const char* const kLevel[] = { "info", "warning", "error" };
    if (!g_ShDebugLog)
        return;
    fprintf(g_ShDebugLog, "[SHARK] %s (line %d): %s\n",
            (unsigned)level < 3 ? kLevel[level] : "?", line, msg ? msg : "");
    fflush(g_ShDebugLog);
}

/* newlib reserves its whole heap as one block at startup (default 128 MB);
 * vitaGL's RAM pool is then what is free minus ram_threshold below. The game's
 * own allocations (texture pools, the flashlight view-space table) come from
 * this heap. Starting point for on-device tuning, not a measurement. */
int _newlib_heap_size_user = 160 * 1024 * 1024;

static int s_vglReady = 0;

SDL_GLContext VitaGL_CreateContext(SDL_Window* w)
{
    (void)w;
    if (!s_vglReady)
    {
        /* ram_threshold: RAM vitaGL leaves free for the newlib heap.
         * Tuned later against real memory pressure (map .bss, VRAM tex). */
        /* O3 + fast math. Precision and int shortcuts stay off: the CLUT/VRAM
         * decode does exact index arithmetic in the fragment shader. */
        vglSetupRuntimeShaderCompiler(SHARK_OPT_FAST, GL_TRUE, GL_FALSE, GL_FALSE);
        vglInitExtended(0, VITA_SCREEN_W, VITA_SCREEN_H,
                        16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
        shark_install_log_cb(VitaGL_SharkLog);
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
