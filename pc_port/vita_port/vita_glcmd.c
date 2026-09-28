/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vita_glcmd.c - render thread for vitaGL (after the dedicated GPU thread of
 * Rinnegatamante's CTR: High Octane Vita port).
 *
 * Every GL call the port makes is recorded into a ring buffer on the calling
 * (game) thread -- arguments and the data they point at copied at the moment
 * of the call -- and replayed into vitaGL by one render thread, so vitaGL's
 * per-call work and the swap run beside the game logic of the next frame.
 * Recording captures state at call time, so no renderer global has to be
 * snapshotted. Calls that return something (object creation, queries, shader
 * compiles, readbacks) run on the render thread too, and the caller waits for
 * them; [GLTHREAD] reports how often that happens per frame.
 *
 * vitaGL itself is initialised on the render thread and never called from
 * any other. config.cfg `vita_render_thread = 0` keeps the old direct path. */
#if defined(__vita__)
#include "vita_sdlgl_shim.h"
#include "vita_glcmd_undef.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern FILE* g_ShDebugLog;

#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif

#define VGLR_RING_SIZE (16u << 20)
#define VGLR_ALIGN(x)  (((size_t)(x) + 7u) & ~(size_t)7u)

typedef struct
{
    uint16_t op;
    uint16_t pad;
    uint32_t size;
} CmdHdr;

static int               s_threaded = 0;

/* Frame jobs (vita_frame_packet): DrawAllSplits hands a whole flush to the
 * render thread, which runs the renderer's draw code itself and calls vitaGL
 * directly instead of recording. The recorders therefore ask which thread
 * they run on: TPIDRURO (the per-thread TLS base) is one register read; the
 * thread id is the fallback if the two threads ever report the same value. */
static uint32_t          s_renderTls;
static SceUID            s_renderTid = -1;
static int               s_tlsUnique = 0;
static volatile uint32_t s_jobsSubmitted, s_jobsDone;
static int               s_jobsOn = 0;

static inline uint32_t VGLR_Tls(void)
{
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v;
}

static inline int VGLR_OnRender(void)
{
    return s_tlsUnique ? (VGLR_Tls() == s_renderTls) : (sceKernelGetThreadId() == s_renderTid);
}

static inline int VGLR_Direct(void)
{
    return !s_threaded || VGLR_OnRender();
}

int VGLR_OnRenderThread(void)
{
    return s_threaded && VGLR_OnRender();
}

static void VGLR_JobFenceWait(void);

/* A game-thread GL call waits for pending frame jobs before it touches the
 * recorder's state filters (bind / uniform / query shadows) the job is using. */
#define VGLR_GAME_FENCE() do { if (s_jobsDone != s_jobsSubmitted && !VGLR_OnRender()) VGLR_JobFenceWait(); } while (0)
static uint8_t*          s_ring;
static volatile uint32_t s_wr;          /* published write position (monotonic) */
static volatile uint32_t s_rd;          /* consumer read position (monotonic) */
static uint32_t          s_wrLocal;     /* producer's position incl. unpublished */
static uint32_t          s_pendingSize; /* size of the reserved, uncommitted command */

static SDL_sem*          s_consSem;
static SDL_sem*          s_prodSem;
static volatile int      s_consWaiting;
static volatile int      s_prodWaiting;

static volatile uint32_t s_framesSubmitted;
static volatile uint32_t s_framesDone;

static GLint             s_unpackAlign = 4;

/* Scratch vertex upload: vglAllocFromScratch hands out the payload of a
 * reserved OP_SCRATCH_BUFFERDATA command; vglBufferData commits it. */
static uint8_t*          s_scratchPayload;

unsigned g_vglrSyncCount;
static const char* s_syncLast[4];
static unsigned    s_syncLastN;

/* Ops handled by hand; the generated GL ops start after these. */
#define OP_WRAP_MARKER        0
#define OP_CALL               1
#define OP_SCRATCH_BUFFERDATA 2
#define OP_SWAP               3
#define OP_JOB                4

typedef struct { void (*fn)(void*); } A_Job;

static void VGLR_RunSync(void (*fn)(void*), void* arg, const char* name);

typedef struct { GLenum target; uint32_t bytes; } A_Scratch;

static inline void VGLR_Barrier(void) { __sync_synchronize(); }

static void VGLR_WakeConsumer(void)
{
    VGLR_Barrier();
    if (s_consWaiting)
    {
        s_consWaiting = 0;
        SDL_SemPost(s_consSem);
    }
}

static void VGLR_WakeProducer(void)
{
    VGLR_Barrier();
    if (s_prodWaiting)
    {
        s_prodWaiting = 0;
        SDL_SemPost(s_prodSem);
    }
}

static void VGLR_Publish(void);

/* Producer-side wait for a condition the render thread will make true.
 * Commands are published in batches, so hand over what is queued first. */
#define VGLR_PRODUCER_WAIT_UNTIL(cond)              \
    do {                                            \
        if (!(cond)) VGLR_Publish();                \
        while (!(cond))                             \
        {                                           \
            s_prodWaiting = 1;                      \
            VGLR_Barrier();                         \
            if (cond) { s_prodWaiting = 0; break; } \
            SDL_SemWaitTimeout(s_prodSem, 2);       \
        }                                           \
    } while (0)

/* Publishing per command cost two barriers and, whenever the render thread
 * had caught up and slept, a semaphore post: a kernel call for each of the
 * ~15k commands of a shadowed frame. */
#define VGLR_PUBLISH_BATCH 48
static uint32_t s_unpublished;

static void VGLR_Publish(void)
{
    s_unpublished = 0;
    if (s_wr == s_wrLocal)
        return;
    VGLR_Barrier();
    s_wr = s_wrLocal;
    VGLR_WakeConsumer();
}

static void VGLR_Commit(void);

static void* VGLR_Reserve(uint16_t op, size_t argsSize, size_t payload)
{
    /* Anything the game thread records while a frame job is still running
     * would read or change renderer state under it: wait for the job first.
     * The swap and further jobs queue behind it in order and need no wait. */
    if (s_jobsDone != s_jobsSubmitted && op != OP_SWAP && op != OP_JOB)
        VGLR_JobFenceWait();

    /* A scratch upload left uncommitted (nothing called vglBufferData on it)
     * must not be overwritten by the next command. */
    if (s_scratchPayload)
    {
        A_Scratch* sa = (A_Scratch*)(s_scratchPayload - VGLR_ALIGN(sizeof(A_Scratch)));
        sa->target = GL_ARRAY_BUFFER;
        s_scratchPayload = NULL;
        VGLR_Commit();
    }

    const uint32_t total = (uint32_t)VGLR_ALIGN(sizeof(CmdHdr) + VGLR_ALIGN(argsSize) + payload);
    uint32_t idx = s_wrLocal & (VGLR_RING_SIZE - 1);

    if (idx + total > VGLR_RING_SIZE)
    {
        const uint32_t skip = VGLR_RING_SIZE - idx;
        VGLR_PRODUCER_WAIT_UNTIL(s_wrLocal + skip - s_rd <= VGLR_RING_SIZE);
        CmdHdr* w = (CmdHdr*)(s_ring + idx);
        w->op = OP_WRAP_MARKER;
        w->size = skip;
        s_wrLocal += skip;
        VGLR_Publish();
        idx = 0;
    }

    VGLR_PRODUCER_WAIT_UNTIL(s_wrLocal + total - s_rd <= VGLR_RING_SIZE);

    CmdHdr* h = (CmdHdr*)(s_ring + idx);
    h->op = op;
    h->size = total;
    s_pendingSize = total;
    return (uint8_t*)h + sizeof(CmdHdr);
}

static void VGLR_Commit(void)
{
    s_wrLocal += s_pendingSize;
    s_pendingSize = 0;
    if (++s_unpublished >= VGLR_PUBLISH_BATCH)
        VGLR_Publish();
}

static size_t VGLR_ImageBytes(GLsizei w, GLsizei h, GLenum format, GLenum type)
{
    size_t bpp;
    size_t row;

    switch (type)
    {
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        bpp = 2;
        break;
    default:
        switch (format)
        {
        case GL_RGBA: case GL_BGRA:           bpp = 4; break;
        case GL_RGB:                          bpp = 3; break;
        case GL_RG: case GL_LUMINANCE_ALPHA:  bpp = 2; break;
        default:                              bpp = 1; break;
        }
        if (type == GL_FLOAT) bpp *= 4;
        break;
    }
    row = (size_t)w * bpp;
    row = (row + (size_t)s_unpackAlign - 1) & ~((size_t)s_unpackAlign - 1);
    return row * (size_t)h;
}

/* Unpack alignment changes the byte size of a texture upload's data. */
static void VGLR_TrackPixelStore(GLenum pname, GLint param)
{
    if (pname == GL_UNPACK_ALIGNMENT && (param == 1 || param == 2 || param == 4 || param == 8))
        s_unpackAlign = param;
}

/* ---- recorded-state copy ------------------------------------------------------
 * glGet* / glIsEnabled would otherwise make the game thread wait for the render
 * thread to drain -- a full stop, several times a frame from state save/restore
 * and diagnostics. The recorders keep this copy of the state they have recorded,
 * which is exactly what GL will hold when the query would have run; anything not
 * tracked here still waits. */
typedef struct
{
    GLuint  drawFbo, readFbo, program, vao, arrayBuffer;
    GLenum  activeUnit;
    GLuint  tex2D[32];
    GLfloat clear[4];
    GLint   scissor[4];
    GLint   viewport[4];
    int     scissorValid, viewportValid;
    GLenum  blend[4];
    GLenum  blendEq[2];
    GLenum  depthFunc;
    uint8_t caps[8];       /* blend, depth, scissor, stencil, cull, polygon offset */
} VGLR_Shadow;

static VGLR_Shadow s_sh = {
    0, 0, 0, 0, 0, GL_TEXTURE0, { 0 }, { 0, 0, 0, 0 }, { 0 }, { 0 }, 0, 0,
    { GL_ONE, GL_ZERO, GL_ONE, GL_ZERO }, { GL_FUNC_ADD, GL_FUNC_ADD }, GL_LESS, { 0 }
};

static int VGLR_CapIndex(GLenum cap)
{
    switch (cap)
    {
    case GL_BLEND:               return 0;
    case GL_DEPTH_TEST:          return 1;
    case GL_SCISSOR_TEST:        return 2;
    case GL_STENCIL_TEST:        return 3;
    case GL_CULL_FACE:           return 4;
    case GL_POLYGON_OFFSET_FILL: return 5;
    default:                     return -1;
    }
}

static void VGLR_Shadow_Cap(GLenum cap, int on)
{
    const int i = VGLR_CapIndex(cap);
    if (i >= 0)
        s_sh.caps[i] = (uint8_t)on;
}

static void VGLR_Shadow_BindFramebuffer(GLenum target, GLuint fb)
{
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) s_sh.drawFbo = fb;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) s_sh.readFbo = fb;
}

static void VGLR_Shadow_DeleteNames(GLsizei n, const GLuint* names, GLuint* bound)
{
    GLsizei i;
    for (i = 0; names && i < n; i++)
        if (names[i] != 0 && names[i] == *bound)
            *bound = 0;
}

static void VGLR_Shadow_DeleteTextures(GLsizei n, const GLuint* names)
{
    int u;
    for (u = 0; u < 32; u++)
        VGLR_Shadow_DeleteNames(n, names, &s_sh.tex2D[u]);
}

/* 1 when the copy answers pname (values written to out), 0 = must ask GL. */
static int VGLR_ShadowGetInt(GLenum pname, GLint* out)
{
    int i;
    switch (pname)
    {
    case GL_FRAMEBUFFER_BINDING: /* == GL_DRAW_FRAMEBUFFER_BINDING */
        out[0] = (GLint)s_sh.drawFbo; return 1;
    case GL_READ_FRAMEBUFFER_BINDING: out[0] = (GLint)s_sh.readFbo; return 1;
    case GL_CURRENT_PROGRAM:         out[0] = (GLint)s_sh.program; return 1;
    case GL_VERTEX_ARRAY_BINDING:    out[0] = (GLint)s_sh.vao; return 1;
    case GL_ARRAY_BUFFER_BINDING:    out[0] = (GLint)s_sh.arrayBuffer; return 1;
    case GL_ACTIVE_TEXTURE:          out[0] = (GLint)s_sh.activeUnit; return 1;
    case GL_TEXTURE_BINDING_2D:
        if (s_sh.activeUnit - GL_TEXTURE0 >= 32u) return 0;
        out[0] = (GLint)s_sh.tex2D[s_sh.activeUnit - GL_TEXTURE0]; return 1;
    case GL_UNPACK_ALIGNMENT:        out[0] = s_unpackAlign; return 1;
    case GL_BLEND_SRC_RGB:           out[0] = (GLint)s_sh.blend[0]; return 1;
    case GL_BLEND_DST_RGB:           out[0] = (GLint)s_sh.blend[1]; return 1;
    case GL_BLEND_SRC_ALPHA:         out[0] = (GLint)s_sh.blend[2]; return 1;
    case GL_BLEND_DST_ALPHA:         out[0] = (GLint)s_sh.blend[3]; return 1;
    case GL_BLEND_EQUATION_RGB:      out[0] = (GLint)s_sh.blendEq[0]; return 1;
    case GL_BLEND_EQUATION_ALPHA:    out[0] = (GLint)s_sh.blendEq[1]; return 1;
    case GL_DEPTH_FUNC:              out[0] = (GLint)s_sh.depthFunc; return 1;
    case GL_VIEWPORT:
        if (!s_sh.viewportValid) return 0;
        for (i = 0; i < 4; i++) out[i] = s_sh.viewport[i];
        return 1;
    case GL_SCISSOR_BOX:
        if (!s_sh.scissorValid) return 0;
        for (i = 0; i < 4; i++) out[i] = s_sh.scissor[i];
        return 1;
    default:
        return 0;
    }
}

/* Redundant-uniform filter. Every texture bind re-sends the whole lit
 * uniform set (~35 values), almost all unchanged; each costs a ring command
 * here and, on the render thread, a dirty uniform buffer the next draw
 * re-uploads. Values persist per program in GL, so a value already held by
 * (program, location) is dropped. Cleared on link/delete (names get reused). */
#define VGLR_UCACHE_SIZE 2048
typedef struct { GLuint prog; GLint loc; uint32_t len; uint8_t data[64]; } VGLR_UEntry;
static VGLR_UEntry s_ucache[VGLR_UCACHE_SIZE];

static void VGLR_UniformCacheClear(void)
{
    memset(s_ucache, 0, sizeof(s_ucache));
}

static int VGLR_UniformSame(GLint loc, const void* data, uint32_t len)
{
    if (loc < 0 || len > sizeof(s_ucache[0].data))
        return 0;
    const GLuint prog = s_sh.program;
    uint32_t h = ((uint32_t)prog * 2654435761u) ^ ((uint32_t)loc * 40503u);
    for (int i = 0; i < 8; i++)
    {
        VGLR_UEntry* e = &s_ucache[(h + i) & (VGLR_UCACHE_SIZE - 1)];
        if (e->len != 0 && (e->prog != prog || e->loc != loc))
            continue;
        if (e->len == len && memcmp(e->data, data, len) == 0)
            return 1;
        e->prog = prog; e->loc = loc; e->len = len;
        memcpy(e->data, data, len);
        return 0;
    }
    VGLR_UEntry* e = &s_ucache[h & (VGLR_UCACHE_SIZE - 1)];
    e->prog = prog; e->loc = loc; e->len = len;
    memcpy(e->data, data, len);
    return 0;
}

/* GPU memory inventory ([VRAMTEX] in VGLR_Report): what our own GL calls
 * allocate, by texture size/format, so a full VRAM pool can be traced to a
 * cache or target. vitaGL's own allocations (display buffers, the implicit depth
 * buffer each FBO gets on first use) are estimated from the FBO attachments. */
#define VGLR_TEXMAX 16384
typedef struct { uint32_t bytes; uint16_t w, h; uint32_t fmt; uint8_t fbo; } VGLR_TexInfo;
static VGLR_TexInfo s_texInfo[VGLR_TEXMAX];
#define VGLR_BUFMAX 256
static struct { GLuint name; uint32_t bytes; } s_bufInfo[VGLR_BUFMAX];
static uint64_t s_rbBytes;

static void VGLR_MemTex(GLint level, uint32_t fmt, GLsizei w, GLsizei h, size_t bytes)
{
    const uint32_t u = s_sh.activeUnit - GL_TEXTURE0;
    if (u >= 32u)
        return;
    const GLuint t = s_sh.tex2D[u];
    if (t == 0 || t >= VGLR_TEXMAX)
        return;
    if (level == 0)
    {
        s_texInfo[t].bytes = (uint32_t)bytes;
        s_texInfo[t].w = (uint16_t)w;
        s_texInfo[t].h = (uint16_t)h;
        s_texInfo[t].fmt = fmt;
    }
    else
        s_texInfo[t].bytes += (uint32_t)bytes;
}

static void VGLR_MemTexDelete(GLsizei n, const GLuint* names)
{
    for (GLsizei i = 0; i < n; i++)
        if (names[i] < VGLR_TEXMAX)
            memset(&s_texInfo[names[i]], 0, sizeof(s_texInfo[0]));
}

static void VGLR_MemTexFbo(GLuint t)
{
    if (t < VGLR_TEXMAX)
        s_texInfo[t].fbo = 1;
}

static void VGLR_MemRb(GLsizei w, GLsizei h)
{
    s_rbBytes += (uint64_t)w * h * 5;
}

static void VGLR_MemBuf(GLuint name, size_t bytes)
{
    int freeSlot = -1;
    for (int i = 0; i < VGLR_BUFMAX; i++)
    {
        if (s_bufInfo[i].name == name) { s_bufInfo[i].bytes = (uint32_t)bytes; return; }
        if (freeSlot < 0 && s_bufInfo[i].name == 0) freeSlot = i;
    }
    if (freeSlot >= 0) { s_bufInfo[freeSlot].name = name; s_bufInfo[freeSlot].bytes = (uint32_t)bytes; }
}

static void VGLR_MemBufDelete(GLsizei n, const GLuint* names)
{
    for (GLsizei i = 0; i < n; i++)
        for (int j = 0; j < VGLR_BUFMAX; j++)
            if (s_bufInfo[j].name == names[i]) s_bufInfo[j].name = 0, s_bufInfo[j].bytes = 0;
}

static const char* VGLR_FmtName(uint32_t f)
{
    switch (f)
    {
    case GL_PALETTE8_RGBA8_OES: return "PAL8";
    case GL_PALETTE4_RGBA8_OES: return "PAL4";
    case GL_RGBA:               return "RGBA";
    case GL_RGB:                return "RGB";
    case GL_LUMINANCE_ALPHA:    return "LA";
    case GL_LUMINANCE:          return "L";
    default:                    return "other";
    }
}

static void VGLR_MemInventory(void)
{
    enum { GROUPS = 32 };
    struct { uint16_t w, h; uint32_t fmt; uint32_t count; uint64_t bytes; } g[GROUPS];
    int ng = 0;
    uint64_t total = 0, fboDepth = 0;
    uint32_t count = 0, fbos = 0;

    for (int t = 1; t < VGLR_TEXMAX; t++)
    {
        const VGLR_TexInfo* e = &s_texInfo[t];
        if (!e->bytes)
            continue;
        count++;
        total += e->bytes;
        if (e->fbo) { fbos++; fboDepth += (uint64_t)e->w * e->h * 5; }
        int k;
        for (k = 0; k < ng; k++)
            if (g[k].w == e->w && g[k].h == e->h && g[k].fmt == e->fmt)
                break;
        if (k == ng)
        {
            if (ng == GROUPS) continue;
            g[ng].w = e->w; g[ng].h = e->h; g[ng].fmt = e->fmt; g[ng].count = 0; g[ng].bytes = 0;
            ng++;
        }
        g[k].count++;
        g[k].bytes += e->bytes;
    }
    uint64_t bufs = 0;
    for (int i = 0; i < VGLR_BUFMAX; i++)
        bufs += s_bufInfo[i].bytes;

    if (!g_ShDebugLog)
        return;
    fprintf(g_ShDebugLog, "[VRAMTEX] %u textures %.1f MB, %u FBO targets (+%.1f MB implicit depth est.), buffers %.1f MB, renderbuffers %.1f MB; largest:",
            count, total / 1048576.0, fbos, fboDepth / 1048576.0, bufs / 1048576.0, s_rbBytes / 1048576.0);
    for (int n = 0; n < 6; n++)
    {
        int best = -1;
        for (int k = 0; k < ng; k++)
            if (g[k].count && (best < 0 || g[k].bytes > g[best].bytes))
                best = k;
        if (best < 0)
            break;
        fprintf(g_ShDebugLog, " %ux%u %s x%u=%.1fMB", g[best].w, g[best].h, VGLR_FmtName(g[best].fmt),
                g[best].count, g[best].bytes / 1048576.0);
        g[best].count = 0;
    }
    fprintf(g_ShDebugLog, "\n");
}

#include "vita_glcmd_gen.inc"

typedef struct
{
    void (*fn)(void*);
    void* arg;
    volatile int* done;
} A_Call;

static void VGLR_RunSync(void (*fn)(void*), void* arg, const char* name)
{
    volatile int done = 0;

    if (VGLR_Direct())
    {
        fn(arg);
        return;
    }

    g_vglrSyncCount++;
    s_syncLast[s_syncLastN++ & 3] = name;

    A_Call* a = (A_Call*)VGLR_Reserve(OP_CALL, sizeof(A_Call), 0);
    a->fn = fn;
    a->arg = arg;
    a->done = &done;
    VGLR_Commit();
    VGLR_PRODUCER_WAIT_UNTIL(done);
}

/* Runs fn on the render thread, in order with the recorded GL calls, and
 * waits for it: for vitaGL entry points the recorder does not wrap. */
void VGLR_Call(void (*fn)(void*), void* arg)
{
    VGLR_RunSync(fn, arg, "VGLR_Call");
}

/* ---- special cases ---------------------------------------------------------- */

typedef struct { GLenum pname; void* data; int kind; GLboolean ret; } S_Query;

static void T_Query(void* p)
{
    S_Query* q = (S_Query*)p;
    switch (q->kind)
    {
    case 0: glGetIntegerv(q->pname, (GLint*)q->data); break;
    case 1: glGetFloatv(q->pname, (GLfloat*)q->data); break;
    case 2: glGetBooleanv(q->pname, (GLboolean*)q->data); break;
    case 3: q->ret = glIsEnabled(q->pname); break;
    }
}

static GLboolean VGLR_Query(GLenum pname, void* data, int kind, const char* name)
{
    S_Query q;
    q.pname = pname; q.data = data; q.kind = kind; q.ret = GL_FALSE;
    VGLR_RunSync(T_Query, &q, name);
    return q.ret;
}

void VGLR_glGetIntegerv(GLenum pname, GLint* data)
{
    VGLR_GAME_FENCE();
    if (s_threaded && VGLR_ShadowGetInt(pname, data))
        return;
    VGLR_Query(pname, data, 0, "glGetIntegerv");
}

void VGLR_glGetFloatv(GLenum pname, GLfloat* data)
{
    VGLR_GAME_FENCE();
    if (s_threaded && pname == GL_COLOR_CLEAR_VALUE)
    {
        memcpy(data, s_sh.clear, sizeof(s_sh.clear));
        return;
    }
    VGLR_Query(pname, data, 1, "glGetFloatv");
}

void VGLR_glGetBooleanv(GLenum pname, GLboolean* data)
{
    VGLR_GAME_FENCE();
    /* Nothing here calls glColorMask, so the mask is the default. */
    if (s_threaded && pname == GL_COLOR_WRITEMASK)
    {
        data[0] = data[1] = data[2] = data[3] = GL_TRUE;
        return;
    }
    VGLR_Query(pname, data, 2, "glGetBooleanv");
}

GLboolean VGLR_glIsEnabled(GLenum cap)
{
    VGLR_GAME_FENCE();
    const int i = VGLR_CapIndex(cap);
    if (s_threaded && i >= 0)
        return s_sh.caps[i] ? GL_TRUE : GL_FALSE;
    return VGLR_Query(cap, NULL, 3, "glIsEnabled");
}

GLenum VGLR_glGetError(void)
{
    /* Waiting on the render thread to report per-frame diagnostics would undo
     * the overlap this whole file exists for. */
    if (VGLR_Direct())
        return glGetError();
    return GL_NO_ERROR;
}

void* VGLR_vglAllocFromScratch(size_t size)
{
    if (VGLR_Direct())
        return vglAllocFromScratch(size);
    A_Scratch* a = (A_Scratch*)VGLR_Reserve(OP_SCRATCH_BUFFERDATA, sizeof(A_Scratch), size);
    a->target = 0;
    a->bytes = (uint32_t)size;
    s_scratchPayload = (uint8_t*)a + VGLR_ALIGN(sizeof(A_Scratch));
    return s_scratchPayload;
}

void VGLR_vglBufferData(GLenum target, const GLvoid* data)
{
    if (VGLR_Direct())
    {
        vglBufferData(target, data);
        return;
    }
    if (data == s_scratchPayload && s_scratchPayload)
    {
        A_Scratch* a = (A_Scratch*)(s_scratchPayload - VGLR_ALIGN(sizeof(A_Scratch)));
        a->target = target;
        s_scratchPayload = NULL;
        VGLR_Commit();
    }
}

/* ---- dynamic resolution ------------------------------------------------------
 * render_scale picks the largest display mode; while the render thread / GPU
 * holds the game thread back at the frame pace, drop one mode (720x408 ->
 * 640x368) and come back once it has been idle for a while. vitaGL applies a
 * resolution change at the next swap with a full GPU finish, so switches are
 * rare and back off when they oscillate. Config key vita_dynamic_res (default 1). */
extern int g_vitaScreenW, g_vitaScreenH;
extern void Vita_ApplySurfaceSize(int w, int h);
#define DYN_WINDOW      30
#define DYN_DROP_US     4000
#define DYN_RAISE_US    500
static int      s_dyn = -1, s_dynLow;
static int      s_dynHiW, s_dynHiH, s_dynLoW, s_dynLoH;
static int      s_dynPendW, s_dynPendH;
static uint32_t s_dynFrames, s_dynCalm, s_dynSince, s_dynHold = 300;
static uint64_t s_dynWaitUs;

static void T_SwapRes(void* p)
{
    const int* wh = (const int*)p;
    vglSwapResolution(wh[0], wh[1]);
}

static void VGLR_DynInit(void)
{
    extern const char* Pc_ModConfig_Value(const char* key);
    const char* cfg = Pc_ModConfig_Value("vita_dynamic_res");
    s_dyn = !(cfg && cfg[0] == '0');
    s_dynHiW = g_vitaScreenW; s_dynHiH = g_vitaScreenH;
    if (s_dynHiW >= 960)      { s_dynLoW = 720; s_dynLoH = 408; }
    else if (s_dynHiW >= 720) { s_dynLoW = 640; s_dynLoH = 368; }
    else                      s_dyn = 0;
    if (g_ShDebugLog)
        fprintf(g_ShDebugLog, "[DYNRES] %s %dx%d <-> %dx%d\n", s_dyn ? "on" : "off",
                s_dynHiW, s_dynHiH, s_dynLoW, s_dynLoH);
}

/* Menus and the inventory draw next to nothing, so the GPU idles and the
 * controller would raise the resolution there and drop it again on the way
 * back, each switch a visible flick. Only live gameplay frames count. */
extern int g_PcWorldDrawnThisFrame;
extern int g_PsxPresentLastFrame;
static uint32_t s_dynHot;

static int VGLR_DynGameplay(void)
{
    return g_PcWorldDrawnThisFrame && !g_PsxPresentLastFrame;
}

static void VGLR_DynUpdate(uint64_t waitUs)
{
    if (!VGLR_DynGameplay())
    {
        s_dynFrames = 0;
        s_dynWaitUs = 0;
        return;
    }
    s_dynWaitUs += waitUs;
    s_dynSince++;
    {
        /* Shadowed flashlight is the heaviest GPU load there is: go to the
         * lower mode once it has been on for ~2 s instead of waiting for the
         * queue to back up (each of those waits was a burst of hitches). */
        extern int g_PsyX_FlashlightActive, g_PsyX_UseFlashlightShadows, g_PsyX_UsePerPixelFlashlight;
        static uint32_t s_flOn;
        const int heavy = g_PsyX_FlashlightActive && g_PsyX_UseFlashlightShadows && g_PsyX_UsePerPixelFlashlight;
        s_flOn = heavy ? s_flOn + 1 : 0;
        if (heavy && !s_dynLow && s_flOn == 60 && s_dynSince >= DYN_WINDOW)
        {
            s_dynLow = 1; s_dynPendW = s_dynLoW; s_dynPendH = s_dynLoH;
            s_dynFrames = 0; s_dynWaitUs = 0;
            if (g_ShDebugLog)
                fprintf(g_ShDebugLog, "[DYNRES] -> %dx%d (shadowed flashlight)\n", s_dynPendW, s_dynPendH);
            return;
        }
        if (heavy && s_dynLow)
        {
            s_dynCalm = 0;	/* stay low while the shadowed flashlight is on */
        }
    }
    if (++s_dynFrames < DYN_WINDOW)
        return;
    const uint64_t avg = s_dynWaitUs / s_dynFrames;
    s_dynFrames = 0;
    s_dynWaitUs = 0;
    if (s_dynSince < s_dynHold)
        return;
    s_dynHot = (avg > DYN_DROP_US) ? s_dynHot + 1 : 0;
    if (!s_dynLow && s_dynHot >= 2)
    {
        /* Dropping again soon after a raise means the raise was wrong. */
        if (s_dynSince < s_dynHold * 3 && s_dynHold < 3600)
            s_dynHold *= 2;
        s_dynLow = 1; s_dynPendW = s_dynLoW; s_dynPendH = s_dynLoH;
    }
    else if (s_dynLow)
    {
        s_dynCalm = (avg < DYN_RAISE_US) ? s_dynCalm + 1 : 0;
        if (s_dynCalm >= 10)
        {
            s_dynLow = 0; s_dynPendW = s_dynHiW; s_dynPendH = s_dynHiH;
        }
    }
    if (s_dynPendW && g_ShDebugLog)
        fprintf(g_ShDebugLog, "[DYNRES] -> %dx%d (wait %.1f ms/frame)\n", s_dynPendW, s_dynPendH, avg / 1000.0);
}

void VGLR_vglSwapBuffers(GLboolean has_commondialog)
{
    if (!s_threaded)
    {
        vglSwapBuffers(has_commondialog);
        return;
    }
    if (s_dyn < 0)
        VGLR_DynInit();

    /* Queued ahead of this frame's swap so the next frame starts at the new size. */
    int applied = 0, wh[2] = { s_dynPendW, s_dynPendH };
    if (s_dynPendW && !VGLR_DynGameplay())
    {
        /* Decided in gameplay but a menu opened: undo the decision. */
        s_dynLow = !s_dynLow;
        s_dynPendW = 0;
    }
    if (s_dynPendW)
    {
        VGLR_RunSync(T_SwapRes, wh, "vglSwapResolution");
        s_dynPendW = 0;
        s_dynSince = 0;
        s_dynCalm = 0;
        applied = 1;
    }

    GLboolean* a = (GLboolean*)VGLR_Reserve(OP_SWAP, sizeof(GLboolean), 0);
    *a = has_commondialog;
    VGLR_Commit();
    VGLR_Publish();
    s_framesSubmitted++;
    /* At most one frame queued behind the one being drawn. */
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    VGLR_PRODUCER_WAIT_UNTIL(s_framesSubmitted - s_framesDone <= 1);
    const uint64_t waited = sceKernelGetProcessTimeWide() - t0;

    if (applied)
    {
        g_vitaScreenW = wh[0];
        g_vitaScreenH = wh[1];
        Vita_ApplySurfaceSize(wh[0], wh[1]);
    }
    else if (s_dyn > 0)
        VGLR_DynUpdate(waited);
}

/* ---- frame jobs ----------------------------------------------------------------- */

static void VGLR_JobFenceWait(void)
{
    VGLR_PRODUCER_WAIT_UNTIL(s_jobsDone == s_jobsSubmitted);
}

int VGLR_JobsEnabled(void)
{
    return s_threaded && s_jobsOn;
}

/* Waits until every submitted frame job has run. No-op on the render thread. */
void VGLR_JobFence(void)
{
    if (!s_threaded || VGLR_OnRender() || s_jobsDone == s_jobsSubmitted)
        return;
    VGLR_JobFenceWait();
}

/* Reserves a job of `bytes` payload in the ring and returns the payload for the
 * caller to fill; VGLR_JobEnd hands it over. NULL = run it inline instead. */
void* VGLR_JobBegin(void (*fn)(void*), size_t bytes)
{
    if (!VGLR_JobsEnabled() || VGLR_OnRender() || bytes > VGLR_RING_SIZE / 4)
        return NULL;
    A_Job* a = (A_Job*)VGLR_Reserve(OP_JOB, sizeof(A_Job), bytes);
    a->fn = fn;
    return (uint8_t*)a + VGLR_ALIGN(sizeof(A_Job));
}

void VGLR_JobEnd(void)
{
    s_jobsSubmitted++;
    VGLR_Commit();
    VGLR_Publish();
}

/* ---- render thread ------------------------------------------------------------ */

static int VGLR_ThreadMain(SceSize args, void* argp)
{
    (void)args; (void)argp;
    s_renderTls = VGLR_Tls();
    VGLR_Barrier();
    s_renderTid = sceKernelGetThreadId();
    for (;;)
    {
        while (s_rd == s_wr)
        {
            s_consWaiting = 1;
            VGLR_Barrier();
            if (s_rd != s_wr) { s_consWaiting = 0; break; }
            SDL_SemWaitTimeout(s_consSem, 5);
        }
        VGLR_Barrier();

        const uint32_t end = s_wr;
        while (s_rd != end)
        {
            const CmdHdr* h = (const CmdHdr*)(s_ring + (s_rd & (VGLR_RING_SIZE - 1)));
            const uint8_t* body = (const uint8_t*)h + sizeof(CmdHdr);
            const uint32_t size = h->size;

            switch (h->op)
            {
            case OP_WRAP_MARKER:
                break;
            case OP_CALL:
            {
                const A_Call* a = (const A_Call*)body;
                a->fn(a->arg);
                VGLR_Barrier();
                *a->done = 1;
                break;
            }
            case OP_SCRATCH_BUFFERDATA:
            {
                const A_Scratch* a = (const A_Scratch*)body;
                void* gpu = vglAllocFromScratch(a->bytes);
                if (gpu)
                {
                    memcpy(gpu, body + VGLR_ALIGN(sizeof(A_Scratch)), a->bytes);
                    vglBufferData(a->target, gpu);
                }
                break;
            }
            case OP_JOB:
            {
                const A_Job* a = (const A_Job*)body;
                a->fn((void*)(body + VGLR_ALIGN(sizeof(A_Job))));
                VGLR_Barrier();
                s_jobsDone++;
                break;
            }
            case OP_SWAP:
                vglSwapBuffers(*(const GLboolean*)body);
                VGLR_Barrier();
                s_framesDone++;
                break;
            default:
                VGLR_Replay(h->op, body);
                break;
            }

            VGLR_Barrier();
            s_rd += size;
            VGLR_WakeProducer();
        }
    }
    return 0;
}

/* Affinity for the helper threads (timing, disc streaming, vitaGL's garbage
 * collector): core 3 when a CoreUnlocker80000H / CapUnlocker plugin lets apps
 * onto the system core, else core 2. Probed once, by moving the calling thread
 * there and back; on a stock Vita the kernel refuses and nothing changes. */
int VGLR_HelperCoreMask(void)
{
    static volatile int s_mask = 0;

    if (!s_mask)
    {
        const SceUID self = sceKernelGetThreadId();
        const int    prev = sceKernelGetThreadCpuAffinityMask(self);
        int          mask = SCE_KERNEL_CPU_MASK_USER_2;

        if (sceKernelChangeThreadCpuAffinityMask(self, SCE_KERNEL_CPU_MASK_SYSTEM) >= 0)
            mask = SCE_KERNEL_CPU_MASK_SYSTEM;
        if (prev > 0)
            sceKernelChangeThreadCpuAffinityMask(self, prev);
        s_mask = mask;
        if (g_ShDebugLog)
            fprintf(g_ShDebugLog, "[CORES] helper threads on core %s\n",
                    mask == SCE_KERNEL_CPU_MASK_SYSTEM ? "3 (unlocked by plugin)" : "2 (core 3 locked)");
    }
    return s_mask;
}

/* Starts the render thread (unless disabled in config.cfg) and runs `init` --
 * vitaGL's initialisation -- on it, waiting until it is done. */
void VGLR_Start(void (*init)(void*))
{
    extern const char* Pc_ModConfig_Value(const char* key);
    const char* cfg = Pc_ModConfig_Value("vita_render_thread");

    if (cfg && cfg[0] == '0')
    {
        if (g_ShDebugLog) fprintf(g_ShDebugLog, "[GLTHREAD] off (config) - direct vitaGL calls\n");
        init(NULL);
        return;
    }

    s_ring = (uint8_t*)malloc(VGLR_RING_SIZE);
    s_consSem = SDL_CreateSemaphore(0);
    s_prodSem = SDL_CreateSemaphore(0);
    if (!s_ring || !s_consSem || !s_prodSem)
    {
        if (g_ShDebugLog) fprintf(g_ShDebugLog, "[GLTHREAD] setup failed - direct vitaGL calls\n");
        init(NULL);
        return;
    }

    /* Core 1: the game thread keeps its own; vitaShaRK compiles on this stack. */
    SceUID th = sceKernelCreateThread("vglRender", VGLR_ThreadMain, 0x10000100, 2 * 1024 * 1024,
                                      0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0)
    {
        if (g_ShDebugLog) fprintf(g_ShDebugLog, "[GLTHREAD] thread start failed - direct vitaGL calls\n");
        init(NULL);
        return;
    }

    /* The caller is the game thread: core 0, leaving core 1 to this one. */
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);

    while (s_renderTid < 0)
        sceKernelDelayThread(100);
    VGLR_Barrier();
    s_tlsUnique = s_renderTls != VGLR_Tls();

    s_threaded = 1;
    VGLR_RunSync(init, NULL, "init");
    if (g_ShDebugLog) fprintf(g_ShDebugLog, "[GLTHREAD] on - vitaGL runs on its own thread (core 1)\n");

    cfg = Pc_ModConfig_Value("vita_frame_packet");
    s_jobsOn = !(cfg && cfg[0] == '0');
    if (g_ShDebugLog)
        fprintf(g_ShDebugLog, "[FRAMEJOB] %s (thread check: %s)\n", s_jobsOn ? "on - draw loop runs on the render thread" : "off (config)",
                s_tlsUnique ? "TPIDRURO" : "thread id");
}

/* Called every 300 frames from the frame report. */
static void T_MemReport(void* p)
{
    size_t* m = (size_t*)p;
    m[0] = vglMemFree(VGL_MEM_RAM);  m[1] = vglMemTotal(VGL_MEM_RAM);
    m[2] = vglMemFree(VGL_MEM_VRAM); m[3] = vglMemTotal(VGL_MEM_VRAM);
}

void VGLR_Report(double frames)
{
    {
        extern int g_cdcStats[2];
        size_t m[4];
        VGLR_RunSync(T_MemReport, m, "vglMemFree");
        if (g_ShDebugLog)
            fprintf(g_ShDebugLog, "[MEM] vitaGL RAM %u/%u MB used, VRAM %u/%u MB used; disc cache hits=%d misses=%d\n",
                    (unsigned)((m[1] - m[0]) >> 20), (unsigned)(m[1] >> 20),
                    (unsigned)((m[3] - m[2]) >> 20), (unsigned)(m[3] >> 20),
                    g_cdcStats[0], g_cdcStats[1]);
        VGLR_MemInventory();
    }
    if (!s_threaded || !g_ShDebugLog)
        return;
    fprintf(g_ShDebugLog, "[GLTHREAD] waits/frame=%.2f last: %s %s %s %s\n",
            g_vglrSyncCount / frames,
            s_syncLast[0] ? s_syncLast[0] : "-", s_syncLast[1] ? s_syncLast[1] : "-",
            s_syncLast[2] ? s_syncLast[2] : "-", s_syncLast[3] ? s_syncLast[3] : "-");
    g_vglrSyncCount = 0;
}
#endif
