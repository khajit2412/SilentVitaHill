/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * vita_sdlgl_shim.h - force-included on PS Vita builds only (-include).
 *
 * vitasdk's SDL2 has no OpenGL context support: it renders through its own
 * GXM backend. vitaGL owns GXM here instead, so SDL keeps input, audio,
 * timers and threads, while every SDL_GL_* call PsyCross makes is routed to
 * vitaGL. Keeping this in one header means the upstream renderer builds for
 * Vita without per-call #ifdefs.
 */
#ifndef VITA_SDLGL_SHIM_H
#define VITA_SDLGL_SHIM_H
#if defined(__vita__)

#ifdef __cplusplus
#include <SDL2/SDL.h>
#include <vitaGL.h>
extern "C" {
#else
#include <SDL2/SDL.h>
#include <vitaGL.h>
#endif

#define VITA_SCREEN_W 960
#define VITA_SCREEN_H 544

SDL_GLContext VitaGL_CreateContext(SDL_Window* w);
void          VitaGL_SwapWindow(SDL_Window* w);
int           VitaGL_SetSwapInterval(int interval);
void          VitaGL_GetDrawableSize(SDL_Window* w, int* x, int* y);
int           VitaGL_GetAttribute(SDL_GLattr attr, int* value);
void*         VitaGL_GetProcAddress(const char* name);

#ifdef __cplusplus
}
#endif

/* The Vita SDL video driver rejects SDL_WINDOW_OPENGL outright ("No OpenGL
 * support in video driver"); a plain window is all vitaGL needs. */
#define SDL_WINDOW_OPENGL              ((SDL_WindowFlags)0)

#define SDL_GL_CreateContext(w)        VitaGL_CreateContext(w)
#define SDL_GL_SwapWindow(w)           VitaGL_SwapWindow(w)
#define SDL_GL_SetSwapInterval(i)      VitaGL_SetSwapInterval(i)
#define SDL_GL_GetDrawableSize(w,x,y)  VitaGL_GetDrawableSize(w,x,y)
#define SDL_GL_GetAttribute(a,v)       VitaGL_GetAttribute(a,v)
#define SDL_GL_GetProcAddress(n)       VitaGL_GetProcAddress(n)
/* Attributes are fixed by vglInit (MSAA mode, stencil); accept and ignore. */
#define SDL_GL_SetAttribute(a,v)       ((void)(a),(void)(v),0)
#define SDL_GL_ResetAttributes()       ((void)0)
#define SDL_GL_DeleteContext(c)        ((void)(c))
#define SDL_GL_MakeCurrent(w,c)        ((void)(w),(void)(c),0)

#endif /* __vita__ */
#endif /* VITA_SDLGL_SHIM_H */
