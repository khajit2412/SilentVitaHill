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

/* The display mode, picked from render_scale when the context is created:
 * 960x544, 720x408, 640x368 or 480x272, all of which the Vita display scales
 * to the panel in hardware. PsyCross sees it as the window size. */
extern int g_vitaScreenW, g_vitaScreenH;
#define VITA_SCREEN_W g_vitaScreenW
#define VITA_SCREEN_H g_vitaScreenH

SDL_GLContext VitaGL_CreateContext(SDL_Window* w);
void          VitaGL_SwapWindow(SDL_Window* w);
int           VitaGL_SetSwapInterval(int interval);
void          VitaGL_GetDrawableSize(SDL_Window* w, int* x, int* y);
int           VitaGL_GetAttribute(SDL_GLattr attr, int* value);
void*         VitaGL_GetProcAddress(const char* name);

/* Per-frame counts of the GL calls that cost a tiled GPU the most (render
 * target switches, clears, readbacks, uploads), reported by PsyX_EndScene.
 * vglSwapBuffers took 20-49 ms/frame even on near-empty screens. */
enum { VGLC_FBO, VGLC_CLEAR, VGLC_READ, VGLC_COPYTEX, VGLC_TEXUP, VGLC_DRAW, VGLC_COUNT };
extern unsigned g_vitaGlCount[VGLC_COUNT];
void VitaGL_Count_BindFramebuffer(GLenum target, GLuint fb);
void VitaGL_Count_Clear(GLbitfield mask);
void VitaGL_Count_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void* p);
void VitaGL_Count_CopyTexSubImage2D(GLenum tg, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h);
void VitaGL_Count_TexSubImage2D(GLenum tg, GLint l, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum f, GLenum t, const void* p);
void VitaGL_Count_TexImage2D(GLenum tg, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum t, const void* p);
void VitaGL_Count_DrawArrays(GLenum m, GLint first, GLsizei n);
void VitaGL_CachedViewport(GLint x, GLint y, GLsizei w, GLsizei h);

/* Redundant-state filter. PsyCross re-sends ~25 uniforms and rebinds its
 * textures on every draw split, almost always to the values already set;
 * dropping those took most of the per-draw cost on vitaGL. */
void VitaGL_UseProgram(GLuint p);
void VitaGL_LinkProgram(GLuint p);
void VitaGL_DeleteProgram(GLuint p);
void VitaGL_Uniform1f(GLint l, GLfloat a);
void VitaGL_Uniform1i(GLint l, GLint a);
void VitaGL_Uniform2f(GLint l, GLfloat a, GLfloat b);
void VitaGL_Uniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d);
void VitaGL_Uniform2fv(GLint l, GLsizei n, const GLfloat* v);
void VitaGL_Uniform3fv(GLint l, GLsizei n, const GLfloat* v);
void VitaGL_UniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat* v);
void VitaGL_ActiveTexture(GLenum unit);
void VitaGL_BindTexture(GLenum target, GLuint tex);
void VitaGL_DeleteTextures(GLsizei n, const GLuint* tex);

#ifdef __cplusplus
}
#endif

#define glBindFramebuffer(t,f)                  VitaGL_Count_BindFramebuffer(t,f)
#define glClear(m)                              VitaGL_Count_Clear(m)
#define glReadPixels(x,y,w,h,f,t,p)             VitaGL_Count_ReadPixels(x,y,w,h,f,t,p)
#define glCopyTexSubImage2D(tg,l,xo,yo,x,y,w,h) VitaGL_Count_CopyTexSubImage2D(tg,l,xo,yo,x,y,w,h)
#define glTexSubImage2D(tg,l,xo,yo,w,h,f,t,p)   VitaGL_Count_TexSubImage2D(tg,l,xo,yo,w,h,f,t,p)
#define glTexImage2D(tg,l,i,w,h,b,f,t,p)        VitaGL_Count_TexImage2D(tg,l,i,w,h,b,f,t,p)
#define glDrawArrays(m,first,n)                 VitaGL_Count_DrawArrays(m,first,n)
#define glViewport(x,y,w,h)                     VitaGL_CachedViewport(x,y,w,h)
#define glUseProgram(p)                         VitaGL_UseProgram(p)
#define glLinkProgram(p)                        VitaGL_LinkProgram(p)
#define glDeleteProgram(p)                      VitaGL_DeleteProgram(p)
#define glUniform1f(l,a)                        VitaGL_Uniform1f(l,a)
#define glUniform1i(l,a)                        VitaGL_Uniform1i(l,a)
#define glUniform2f(l,a,b)                      VitaGL_Uniform2f(l,a,b)
#define glUniform4f(l,a,b,c,d)                  VitaGL_Uniform4f(l,a,b,c,d)
#define glUniform2fv(l,n,v)                     VitaGL_Uniform2fv(l,n,v)
#define glUniform3fv(l,n,v)                     VitaGL_Uniform3fv(l,n,v)
#define glUniformMatrix4fv(l,n,t,v)             VitaGL_UniformMatrix4fv(l,n,t,v)
#define glActiveTexture(u)                      VitaGL_ActiveTexture(u)
#define glBindTexture(t,x)                      VitaGL_BindTexture(t,x)
#define glDeleteTextures(n,x)                   VitaGL_DeleteTextures(n,x)

/* The Vita SDL video driver rejects SDL_WINDOW_OPENGL outright ("No OpenGL
 * support in video driver"); a plain window is all vitaGL needs. */
#define SDL_WINDOW_OPENGL              ((SDL_WindowFlags)0)

#define SDL_GL_CreateContext(w)        VitaGL_CreateContext(w)
#define SDL_GetWindowSize(w,x,y)       VitaGL_GetDrawableSize(w,x,y)
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
