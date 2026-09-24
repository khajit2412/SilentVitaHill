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

int g_vitaScreenW = 960, g_vitaScreenH = 544;

/* render_scale as a display mode instead of an offscreen target: the Vita
 * scales these framebuffers to the panel for free, where the target needed a
 * glBlitFramebuffer stretch that vitaGL did not do (the scene sat in a corner). */
static void VitaGL_PickDisplayMode(void)
{
    extern float g_cfg_renderScale;
    const float s = g_cfg_renderScale;

    if (s >= 0.95f)      { g_vitaScreenW = 960; g_vitaScreenH = 544; }
    else if (s >= 0.70f) { g_vitaScreenW = 720; g_vitaScreenH = 408; }
    else if (s >= 0.60f) { g_vitaScreenW = 640; g_vitaScreenH = 368; }
    else                 { g_vitaScreenW = 480; g_vitaScreenH = 272; }
}

#undef glBindFramebuffer
#undef glClear
#undef glReadPixels
#undef glCopyTexSubImage2D
#undef glTexSubImage2D
#undef glTexImage2D
#undef glDrawArrays
#undef glViewport
#undef glUseProgram
#undef glLinkProgram
#undef glDeleteProgram
#undef glUniform1f
#undef glUniform1i
#undef glUniform2f
#undef glUniform4f
#undef glUniform2fv
#undef glUniform3fv
#undef glUniformMatrix4fv
#undef glActiveTexture
#undef glBindTexture
#undef glDeleteTextures

/* ---- redundant-state filter ------------------------------------------------
 * Uniform values belong to a program, so the cache is keyed by (program,
 * location) and emptied whenever any program is linked or deleted (values
 * reset, ids get reused). Texture bindings are tracked per unit for
 * GL_TEXTURE_2D; a deleted texture is unbound by GL, so it is forgotten. */
#define UCACHE_SIZE 2048
typedef struct { GLuint prog; GLint loc; unsigned char n; float v[16]; } UEntry;
static UEntry s_ucache[UCACHE_SIZE];
static GLuint s_curProg = (GLuint)-1;

static void UCache_Clear(void)
{
    memset(s_ucache, 0, sizeof(s_ucache));
}

/* 1 when (current program, loc) already holds these n floats; else records them. */
static int UCache_Same(GLint loc, const float* v, unsigned n)
{
    unsigned h, i;
    UEntry*  e = NULL;

    if (loc == -1 || s_curProg == (GLuint)-1 || s_curProg == 0)
        return 0;
    h = ((unsigned)s_curProg * 2654435761u) ^ ((unsigned)loc * 40503u);
    for (i = 0; i < 8; i++)
    {
        UEntry* c = &s_ucache[(h + i) & (UCACHE_SIZE - 1)];
        if (c->n != 0 && c->prog == s_curProg && c->loc == loc)
        {
            e = c;
            break;
        }
        if (c->n == 0 && e == NULL)
            e = c;
    }
    if (e == NULL)
        e = &s_ucache[h & (UCACHE_SIZE - 1)];
    if (e->n == n && e->prog == s_curProg && e->loc == loc && memcmp(e->v, v, n * sizeof(float)) == 0)
        return 1;
    e->prog = s_curProg;
    e->loc  = loc;
    e->n    = (unsigned char)n;
    memcpy(e->v, v, n * sizeof(float));
    return 0;
}

void VitaGL_UseProgram(GLuint p)
{
    if (p == s_curProg)
        return;
    s_curProg = p;
    glUseProgram(p);
}

void VitaGL_LinkProgram(GLuint p)
{
    UCache_Clear();
    glLinkProgram(p);
}

void VitaGL_DeleteProgram(GLuint p)
{
    UCache_Clear();
    if (p == s_curProg)
        s_curProg = (GLuint)-1;
    glDeleteProgram(p);
}

void VitaGL_Uniform1f(GLint l, GLfloat a)
{
    if (!UCache_Same(l, &a, 1)) glUniform1f(l, a);
}

void VitaGL_Uniform1i(GLint l, GLint a)
{
    float f;
    memcpy(&f, &a, sizeof(f));
    if (!UCache_Same(l, &f, 1)) glUniform1i(l, a);
}

void VitaGL_Uniform2f(GLint l, GLfloat a, GLfloat b)
{
    const float v[2] = { a, b };
    if (!UCache_Same(l, v, 2)) glUniform2f(l, a, b);
}

void VitaGL_Uniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d)
{
    const float v[4] = { a, b, c, d };
    if (!UCache_Same(l, v, 4)) glUniform4f(l, a, b, c, d);
}

void VitaGL_Uniform2fv(GLint l, GLsizei n, const GLfloat* v)
{
    if (n != 1 || !UCache_Same(l, v, 2)) glUniform2fv(l, n, v);
}

void VitaGL_Uniform3fv(GLint l, GLsizei n, const GLfloat* v)
{
    if (n != 1 || !UCache_Same(l, v, 3)) glUniform3fv(l, n, v);
}

void VitaGL_UniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat* v)
{
    if (n != 1 || t || !UCache_Same(l, v, 16)) glUniformMatrix4fv(l, n, t, v);
}

static GLenum s_activeUnit = 0;
static GLuint s_bound2D[32];
static int    s_texCacheValid = 0;

void VitaGL_ActiveTexture(GLenum unit)
{
    if (s_texCacheValid && unit == s_activeUnit)
        return;
    s_activeUnit = unit;
    glActiveTexture(unit);
}

void VitaGL_BindTexture(GLenum target, GLuint tex)
{
    const unsigned u = (unsigned)(s_activeUnit - GL_TEXTURE0);

    if (target != GL_TEXTURE_2D || u >= 32 || !s_texCacheValid)
    {
        glBindTexture(target, tex);
        return;
    }
    if (s_bound2D[u] == tex)
        return;
    s_bound2D[u] = tex;
    glBindTexture(target, tex);
}

void VitaGL_DeleteTextures(GLsizei n, const GLuint* tex)
{
    GLsizei i;
    unsigned u;

    for (i = 0; i < n; i++)
        for (u = 0; u < 32; u++)
            if (s_bound2D[u] == tex[i])
                s_bound2D[u] = 0;
    glDeleteTextures(n, tex);
}

/* PsyCross sets the viewport on every draw split, mostly to what it already
 * is. Forgotten on any framebuffer bind in case vitaGL ties it to the target. */
static GLint s_vp[4] = { -1, -1, -1, -1 };

void VitaGL_CachedViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (s_vp[0] == x && s_vp[1] == y && s_vp[2] == w && s_vp[3] == h)
        return;
    s_vp[0] = x; s_vp[1] = y; s_vp[2] = w; s_vp[3] = h;
    glViewport(x, y, w, h);
}

unsigned g_vitaGlCount[VGLC_COUNT];

void VitaGL_Count_BindFramebuffer(GLenum target, GLuint fb)
{
    g_vitaGlCount[VGLC_FBO]++;
    s_vp[0] = -1;
    glBindFramebuffer(target, fb);
}

void VitaGL_Count_Clear(GLbitfield mask)
{
    g_vitaGlCount[VGLC_CLEAR]++;
    glClear(mask);
}

void VitaGL_Count_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void* p)
{
    g_vitaGlCount[VGLC_READ]++;
    glReadPixels(x, y, w, h, f, t, p);
}

void VitaGL_Count_CopyTexSubImage2D(GLenum tg, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
    g_vitaGlCount[VGLC_COPYTEX]++;
    glCopyTexSubImage2D(tg, l, xo, yo, x, y, w, h);
}

void VitaGL_Count_TexSubImage2D(GLenum tg, GLint l, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum f, GLenum t, const void* p)
{
    g_vitaGlCount[VGLC_TEXUP]++;
    glTexSubImage2D(tg, l, xo, yo, w, h, f, t, p);
}

void VitaGL_Count_TexImage2D(GLenum tg, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum t, const void* p)
{
    g_vitaGlCount[VGLC_TEXUP]++;
    glTexImage2D(tg, l, ifmt, w, h, b, f, t, p);
}

void VitaGL_Count_DrawArrays(GLenum m, GLint first, GLsizei n)
{
    g_vitaGlCount[VGLC_DRAW]++;
    glDrawArrays(m, first, n);
}

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
        VitaGL_PickDisplayMode();
        vglInitExtended(0, VITA_SCREEN_W, VITA_SCREEN_H,
                        16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
        shark_install_log_cb(VitaGL_SharkLog);
        /* A fresh context: unit 0 active, nothing bound. */
        s_activeUnit    = GL_TEXTURE0;
        memset(s_bound2D, 0, sizeof(s_bound2D));
        s_texCacheValid = 1;
        /* Off until the config says otherwise (GR_UpdateSwapIntervalState):
         * the game paces itself to 30 fps. */
        vglWaitVblankStart(GL_FALSE);
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
