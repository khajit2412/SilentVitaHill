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

/* Producer-side wait for a condition the render thread will make true. */
#define VGLR_PRODUCER_WAIT_UNTIL(cond)              \
    do {                                            \
        while (!(cond))                             \
        {                                           \
            s_prodWaiting = 1;                      \
            VGLR_Barrier();                         \
            if (cond) { s_prodWaiting = 0; break; } \
            SDL_SemWaitTimeout(s_prodSem, 2);       \
        }                                           \
    } while (0)

static void VGLR_Publish(void)
{
    VGLR_Barrier();
    s_wr = s_wrLocal;
    VGLR_WakeConsumer();
}

static void VGLR_Commit(void);

static void* VGLR_Reserve(uint16_t op, size_t argsSize, size_t payload)
{
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

    if (!s_threaded)
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
    if (s_threaded && VGLR_ShadowGetInt(pname, data))
        return;
    VGLR_Query(pname, data, 0, "glGetIntegerv");
}

void VGLR_glGetFloatv(GLenum pname, GLfloat* data)
{
    if (s_threaded && pname == GL_COLOR_CLEAR_VALUE)
    {
        memcpy(data, s_sh.clear, sizeof(s_sh.clear));
        return;
    }
    VGLR_Query(pname, data, 1, "glGetFloatv");
}

void VGLR_glGetBooleanv(GLenum pname, GLboolean* data)
{
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
    const int i = VGLR_CapIndex(cap);
    if (s_threaded && i >= 0)
        return s_sh.caps[i] ? GL_TRUE : GL_FALSE;
    return VGLR_Query(cap, NULL, 3, "glIsEnabled");
}

GLenum VGLR_glGetError(void)
{
    /* Waiting on the render thread to report per-frame diagnostics would undo
     * the overlap this whole file exists for. */
    if (!s_threaded)
        return glGetError();
    return GL_NO_ERROR;
}

void* VGLR_vglAllocFromScratch(size_t size)
{
    if (!s_threaded)
        return vglAllocFromScratch(size);
    A_Scratch* a = (A_Scratch*)VGLR_Reserve(OP_SCRATCH_BUFFERDATA, sizeof(A_Scratch), size);
    a->target = 0;
    a->bytes = (uint32_t)size;
    s_scratchPayload = (uint8_t*)a + VGLR_ALIGN(sizeof(A_Scratch));
    return s_scratchPayload;
}

void VGLR_vglBufferData(GLenum target, const GLvoid* data)
{
    if (!s_threaded)
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

void VGLR_vglSwapBuffers(GLboolean has_commondialog)
{
    if (!s_threaded)
    {
        vglSwapBuffers(has_commondialog);
        return;
    }
    GLboolean* a = (GLboolean*)VGLR_Reserve(OP_SWAP, sizeof(GLboolean), 0);
    *a = has_commondialog;
    VGLR_Commit();
    s_framesSubmitted++;
    /* At most one frame queued behind the one being drawn. */
    VGLR_PRODUCER_WAIT_UNTIL(s_framesSubmitted - s_framesDone <= 1);
}

/* ---- render thread ------------------------------------------------------------ */

static int VGLR_ThreadMain(SceSize args, void* argp)
{
    (void)args; (void)argp;
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

    s_threaded = 1;
    VGLR_RunSync(init, NULL, "init");
    if (g_ShDebugLog) fprintf(g_ShDebugLog, "[GLTHREAD] on - vitaGL runs on its own thread (core 1)\n");
}

/* Called every 300 frames from the frame report. */
void VGLR_Report(double frames)
{
    if (!s_threaded || !g_ShDebugLog)
        return;
    fprintf(g_ShDebugLog, "[GLTHREAD] waits/frame=%.2f last: %s %s %s %s\n",
            g_vglrSyncCount / frames,
            s_syncLast[0] ? s_syncLast[0] : "-", s_syncLast[1] ? s_syncLast[1] : "-",
            s_syncLast[2] ? s_syncLast[2] : "-", s_syncLast[3] ? s_syncLast[3] : "-");
    g_vglrSyncCount = 0;
}
#endif
