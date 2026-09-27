#!/usr/bin/env python3
# Generates vita_glcmd_gen.inc / vita_glcmd_redirect.h from vitaGL's prototypes.
# Run: python3 gen_glcmd.py /usr/local/vitasdk/arm-vita-eabi/include/vitaGL.h
import re, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))

VALUE = """glActiveTexture glAttachShader glBindBuffer glBindFramebuffer glBindRenderbuffer glBindTexture
glBindVertexArray glBlendEquation glBlendEquationSeparate glBlendFunc glBlendFuncSeparate glBlitFramebuffer
glClear glClearColor glClearDepthf glCompileShader glCopyTexSubImage2D glDeleteProgram glDeleteShader
glDepthFunc glDepthMask glDisable glDrawArrays glEnable glEnableVertexAttribArray glFramebufferRenderbuffer
glFramebufferTexture2D glGenerateMipmap glLineWidth glLinkProgram glPolygonOffset glReadBuffer
glRenderbufferStorage glScissor glStencilFunc glStencilOp glTexParameterf glTexParameteri glUniform1f
glUniform1i glUniform2f glUniform4f glUseProgram glViewport glVertexAttribPointer vglWaitVblankStart
glPixelStorei""".split()

# Extra producer-side statement at the top of a recorder (before the threaded check).
HOOKS = {
    "glPixelStorei":           "VGLR_TrackPixelStore(pname, param);",
    "glBindFramebuffer":       "VGLR_Shadow_BindFramebuffer(target, framebuffer);",
    "glUseProgram":            "s_sh.program = program;",
    "glBindVertexArray":       "s_sh.vao = array;",
    "glBindBuffer":            "if (target == GL_ARRAY_BUFFER) s_sh.arrayBuffer = buffer;",
    "glActiveTexture":         "s_sh.activeUnit = texture;",
    "glBindTexture":           "if (target == GL_TEXTURE_2D && s_sh.activeUnit - GL_TEXTURE0 < 32u) s_sh.tex2D[s_sh.activeUnit - GL_TEXTURE0] = texture;",
    "glClearColor":            "s_sh.clear[0] = red; s_sh.clear[1] = green; s_sh.clear[2] = blue; s_sh.clear[3] = alpha;",
    "glScissor":               "s_sh.scissor[0] = x; s_sh.scissor[1] = y; s_sh.scissor[2] = width; s_sh.scissor[3] = height; s_sh.scissorValid = 1;",
    "glViewport":              "s_sh.viewport[0] = x; s_sh.viewport[1] = y; s_sh.viewport[2] = width; s_sh.viewport[3] = height; s_sh.viewportValid = 1;",
    "glBlendFunc":             "s_sh.blend[0] = s_sh.blend[2] = sfactor; s_sh.blend[1] = s_sh.blend[3] = dfactor;",
    "glBlendFuncSeparate":     "s_sh.blend[0] = srcRGB; s_sh.blend[1] = dstRGB; s_sh.blend[2] = srcAlpha; s_sh.blend[3] = dstAlpha;",
    "glBlendEquation":         "s_sh.blendEq[0] = s_sh.blendEq[1] = mode;",
    "glBlendEquationSeparate": "s_sh.blendEq[0] = modeRGB; s_sh.blendEq[1] = modeAlpha;",
    "glDepthFunc":             "s_sh.depthFunc = func;",
    "glEnable":                "VGLR_Shadow_Cap(cap, 1);",
    "glDisable":               "VGLR_Shadow_Cap(cap, 0);",
    "glDeleteTextures":        "VGLR_Shadow_DeleteTextures(n, textures);",
    "glDeleteBuffers":         "VGLR_Shadow_DeleteNames(n, gl_buffers, &s_sh.arrayBuffer);",
    "glDeleteVertexArrays":    "VGLR_Shadow_DeleteNames(n, gl_arrays, &s_sh.vao);",
    "glDeleteFramebuffers":    "VGLR_Shadow_DeleteNames(n, framebuffers, &s_sh.drawFbo); VGLR_Shadow_DeleteNames(n, framebuffers, &s_sh.readFbo);",
    "glDeleteProgram":         "if (s_sh.program == prog) s_sh.program = 0; VGLR_UniformCacheClear();",
    "glLinkProgram":           "VGLR_UniformCacheClear();",
    "glUniform1f":             "if (VGLR_UniformSame(location, &v0, 4)) return;",
    "glUniform1i":             "if (VGLR_UniformSame(location, &v0, 4)) return;",
    "glUniform2f":             "{ const GLfloat t[2] = { v0, v1 }; if (VGLR_UniformSame(location, t, 8)) return; }",
    "glUniform4f":             "{ const GLfloat t[4] = { v0, v1, v2, v3 }; if (VGLR_UniformSame(location, t, 16)) return; }",
    "glUniform1iv":            "if (count == 1 && VGLR_UniformSame(location, value, 4)) return;",
    "glUniform2fv":            "if (count == 1 && VGLR_UniformSame(location, value, 8)) return;",
    "glUniform3fv":            "if (count == 1 && VGLR_UniformSame(location, value, 12)) return;",
    "glUniformMatrix4fv":      "if (count == 1 && !transpose && VGLR_UniformSame(location, value, 64)) return;",
}

# name -> (pointer param, C expression for payload bytes; may use params)
PAYLOAD = {
    "glUniform1iv":        ("value", "(size_t)count * 4"),
    "glUniform2fv":        ("value", "(size_t)count * 8"),
    "glUniform3fv":        ("value", "(size_t)count * 12"),
    "glUniformMatrix4fv":  ("value", "(size_t)count * 64"),
    "glDeleteTextures":    ("textures", "(size_t)n * 4"),
    "glDeleteBuffers":     ("gl_buffers", "(size_t)n * 4"),
    "glDeleteVertexArrays":("gl_arrays", "(size_t)n * 4"),
    "glDeleteFramebuffers":("framebuffers", "(size_t)n * 4"),
    "glDeleteRenderbuffers":("renderbuffers", "(size_t)n * 4"),
    "glBufferData":        ("data", "(data ? (size_t)size : 0)"),
    "glBufferSubData":     ("data", "(data ? (size_t)size : 0)"),
    "glCompressedTexImage2D": ("data", "(data ? (size_t)imageSize : 0)"),
    "glTexImage2D":        ("data", "(data ? VGLR_ImageBytes(width, height, format, type) : 0)"),
    "glTexSubImage2D":     ("pixels", "(pixels ? VGLR_ImageBytes(width, height, format, type) : 0)"),
}

SYNC = """glGenTextures glGenBuffers glGenVertexArrays glGenFramebuffers glGenRenderbuffers
glGetUniformLocation glCreateShader glCreateProgram glGetShaderiv
glGetShaderInfoLog glGetProgramiv glGetProgramInfoLog glGetString glGetStringi glIsTexture glIsProgram
glCheckFramebufferStatus glGetFramebufferAttachmentParameteriv glReadPixels glFinish glShaderSource
glBindAttribLocation""".split()

SPECIAL = ["glGetError", "vglAllocFromScratch", "vglBufferData", "vglSwapBuffers",
           "glGetIntegerv", "glGetFloatv", "glGetBooleanv", "glIsEnabled"]

def parse(h):
    protos = {}
    for n in VALUE + list(PAYLOAD) + SYNC + SPECIAL:
        m = re.search(r'^([\w\s\*]+?)\b' + n + r'\s*\(([^;]*?)\);', h, re.M)
        if not m:
            sys.exit("missing prototype: " + n)
        ret = ' '.join(m.group(1).split())
        args = ' '.join(m.group(2).split())
        params = []
        if args != 'void':
            for a in args.split(','):
                a = a.strip()
                mm = re.match(r'(.*?)(\w+)$', a)
                params.append((mm.group(1).strip(), mm.group(2)))
        protos[n] = (ret, params)
    return protos

def decl(ret, n, params):
    ps = ', '.join(f"{t} {p}" for t, p in params) or 'void'
    return f"{ret} VGLR_{n}({ps})"

def main():
    h = open(sys.argv[1]).read()
    P = parse(h)
    names = VALUE + list(PAYLOAD) + SYNC
    ops = VALUE + list(PAYLOAD)

    redir = ["/* Generated by gen_glcmd.py. Included by vita_sdlgl_shim.h (no include guard:",
             " * vita_sdlgl_shim.c re-includes it after its own #undefs). */"]
    for n in names + SPECIAL:
        redir.append(f"#undef {n}\n#define {n} VGLR_{n}")
    open(os.path.join(HERE, "vita_glcmd_redirect.h"), "w").write('\n'.join(redir) + '\n')
    undef = ["/* Generated by gen_glcmd.py: exposes the real vitaGL entry points. */"]
    undef += [f"#undef {n}" for n in names + SPECIAL]
    open(os.path.join(HERE, "vita_glcmd_undef.h"), "w").write('\n'.join(undef) + '\n')

    out = ["/* Generated by gen_glcmd.py from vitaGL.h -- do not edit. */", ""]
    # prototypes header part
    protos = ["/* Generated by gen_glcmd.py -- do not edit. */"]
    for n in names + SPECIAL:
        ret, params = P[n]
        protos.append(decl(ret, n, params) + ";")
    open(os.path.join(HERE, "vita_glcmd_protos.h"), "w").write('\n'.join(protos) + '\n')

    out.append("enum {\n    OP_GL_FIRST = 15,")
    for n in ops:
        out.append(f"    OP_{n},")
    out.append("    OP_COUNT\n};\n")


    # arg structs + recorders + replay for VALUE and PAYLOAD
    replay = ["static void VGLR_Replay(uint16_t op, const uint8_t* body)\n{\n    switch (op)\n    {"]
    for n in ops:
        ret, params = P[n]
        fields = []
        for t, p in params:
            if n in PAYLOAD and p == PAYLOAD[n][0]:
                fields.append(f"    uint32_t {p}_bytes; int32_t {p}_null;")
            elif '*' in t:
                fields.append(f"    uintptr_t {p};")
            else:
                fields.append(f"    {t} {p};")
        out.append(f"typedef struct {{\n" + ('\n'.join(fields) if fields else "    int unused;") + f"\n}} A_{n};\n")
        # recorder
        out.append(decl(ret, n, params) + "\n{")
        call = ', '.join(p for t, p in params)
        if n in HOOKS:
            out.append("    " + HOOKS[n])
        out.append(f"    if (!s_threaded) {{ {n}({call}); return; }}")
        if n in PAYLOAD:
            ptr, expr = PAYLOAD[n]
            out.append(f"    const size_t pay = {expr};")
            out.append(f"    A_{n}* a = (A_{n}*)VGLR_Reserve(OP_{n}, sizeof(A_{n}), pay);")
        else:
            out.append(f"    A_{n}* a = (A_{n}*)VGLR_Reserve(OP_{n}, sizeof(A_{n}), 0);")
        for t, p in params:
            if n in PAYLOAD and p == PAYLOAD[n][0]:
                out.append(f"    a->{p}_bytes = (uint32_t)pay; a->{p}_null = ({p} == NULL);")
                out.append(f"    if (pay) memcpy((uint8_t*)a + VGLR_ALIGN(sizeof(A_{n})), {p}, pay);")
            elif '*' in t:
                out.append(f"    a->{p} = (uintptr_t){p};")
            else:
                out.append(f"    a->{p} = {p};")
        out.append("    VGLR_Commit();\n}\n")
        # replay
        args = []
        for t, p in params:
            if n in PAYLOAD and p == PAYLOAD[n][0]:
                args.append(f"(a->{p}_null ? NULL : (const void*)(body + VGLR_ALIGN(sizeof(A_{n}))))")
            elif '*' in t:
                args.append(f"({t})a->{p}")
            else:
                args.append(f"a->{p}")
        replay.append(f"    case OP_{n}: {{ const A_{n}* a = (const A_{n}*)body; (void)a; {n}({', '.join(args)}); break; }}")
    replay.append("    default: break;\n    }\n}\n")

    # SYNC: thunks
    for n in SYNC:
        ret, params = P[n]
        fields = [f"    {t} {p};" for t, p in params]
        if ret != 'void':
            fields.append(f"    {ret} ret;")
        out.append(f"typedef struct {{\n" + ('\n'.join(fields) if fields else "    int unused;") + f"\n}} S_{n};\n")
        call = ', '.join(f"s->{p}" for t, p in params)
        body = (f"s->ret = {n}({call});" if ret != 'void' else f"{n}({call});")
        out.append(f"static void T_{n}(void* p) {{ S_{n}* s = (S_{n}*)p; {body} }}\n")
        out.append(decl(ret, n, params) + "\n{")
        out.append(f"    S_{n} s;")
        for t, p in params:
            out.append(f"    s.{p} = {p};")
        out.append(f"    VGLR_RunSync(T_{n}, &s, \"{n}\");")
        if ret != 'void':
            out.append("    return s.ret;")
        out.append("}\n")

    out += replay
    open(os.path.join(HERE, "vita_glcmd_gen.inc"), "w").write('\n'.join(out) + '\n')

main()
