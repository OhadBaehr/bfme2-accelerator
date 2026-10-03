"""Generates aotr_rt_gen.inc: one hook function per COM method slot for every interface the render thread
(RT) intercepts, plus the per-class original/hook tables. Spec format per slot: (name, argspec, kind)
  kind: A = queue asynchronously (arguments + pointed-to data copied now, executed in order on the worker)
        S = synchronous (wait for the worker to finish everything queued, then run on the game thread)
        D = direct (immutable reads / atomic refcounts: safe to run concurrently)
        R = release (take a temporary reference now, queue the releases so destruction happens in order)
        L = direct under the exec lock without draining: results that no queued call can change
            (object identity, swap chain / back buffer / display mode, device-lost state)
        X = hand-written in aotr_rt.inc (declared, not generated)
  argspec: one token per argument after `this`:
        v        plain value
        o        COM object (AddRef when queued, Release after execution)
        p<N>     pointer to N bytes (may be NULL)
        pc<M>@i  pointer to (value of argument i) * M bytes (may be NULL)
        h        D3DXHANDLE (a string name handle is copied)
        s        C string
"""
import os

DEV = [
    ("QueryInterface", "vv", "S"), ("AddRef", "", "D"), ("Release", "", "X"), ("TestCooperativeLevel", "", "L"),
    ("GetAvailableTextureMem", "", "S"), ("EvictManagedResources", "", "A"), ("GetDirect3D", "v", "D"),
    ("GetDeviceCaps", "v", "D"), ("GetDisplayMode", "vv", "L"), ("GetCreationParameters", "v", "D"),
    ("SetCursorProperties", "vvv", "S"), ("SetCursorPosition", "vvv", "S"), ("ShowCursor", "v", "S"),
    ("CreateAdditionalSwapChain", "vv", "S"), ("GetSwapChain", "vv", "L"), ("GetNumberOfSwapChains", "", "D"),
    ("Reset", "v", "X"), ("Present", "vvvv", "X"), ("GetBackBuffer", "vvvv", "L"), ("GetRasterStatus", "vv", "L"),
    ("SetDialogBoxMode", "v", "S"), ("SetGammaRamp", "vvp1536", "A"), ("GetGammaRamp", "vv", "S"),
    ("CreateTexture", "vvvvvvvv", "X"), ("CreateVolumeTexture", "vvvvvvvvv", "L"), ("CreateCubeTexture", "vvvvvvv", "L"),
    ("CreateVertexBuffer", "vvvvvv", "L"), ("CreateIndexBuffer", "vvvvvv", "L"), ("CreateRenderTarget", "vvvvvvvv", "L"),
    ("CreateDepthStencilSurface", "vvvvvvvv", "L"), ("UpdateSurface", "op16op8", "X"), ("UpdateTexture", "oo", "A"),
    ("GetRenderTargetData", "vv", "S"), ("GetFrontBufferData", "vv", "S"), ("StretchRect", "op16op16v", "A"),
    ("ColorFill", "op16v", "A"), ("CreateOffscreenPlainSurface", "vvvvvv", "L"), ("SetRenderTarget", "vo", "X"),
    ("GetRenderTarget", "vv", "X"), ("SetDepthStencilSurface", "o", "X"), ("GetDepthStencilSurface", "v", "X"),
    ("BeginScene", "", "A"), ("EndScene", "", "A"), ("Clear", "vpc16@0vvvv", "A"), ("SetTransform", "vp64", "A"),
    ("GetTransform", "vv", "S"), ("MultiplyTransform", "vp64", "A"), ("SetViewport", "p24", "A"), ("GetViewport", "v", "S"),
    ("SetMaterial", "p68", "A"), ("GetMaterial", "v", "S"), ("SetLight", "vp104", "A"), ("GetLight", "vv", "S"),
    ("LightEnable", "vv", "A"), ("GetLightEnable", "vv", "S"), ("SetClipPlane", "vp16", "A"), ("GetClipPlane", "vv", "S"),
    ("SetRenderState", "vv", "A"), ("GetRenderState", "vv", "S"), ("CreateStateBlock", "vv", "S"), ("BeginStateBlock", "", "S"),
    ("EndStateBlock", "v", "S"), ("SetClipStatus", "p8", "A"), ("GetClipStatus", "v", "S"), ("GetTexture", "vv", "S"),
    ("SetTexture", "vo", "A"), ("GetTextureStageState", "vvv", "S"), ("SetTextureStageState", "vvv", "A"),
    ("GetSamplerState", "vvv", "S"), ("SetSamplerState", "vvv", "A"), ("ValidateDevice", "v", "S"),
    ("SetPaletteEntries", "vp1024", "A"), ("GetPaletteEntries", "vv", "S"), ("SetCurrentTexturePalette", "v", "A"),
    ("GetCurrentTexturePalette", "v", "S"), ("SetScissorRect", "p16", "A"), ("GetScissorRect", "v", "S"),
    ("SetSoftwareVertexProcessing", "v", "A"), ("GetSoftwareVertexProcessing", "", "S"), ("SetNPatchMode", "v", "A"),
    ("GetNPatchMode", "", "X"), ("DrawPrimitive", "vvv", "A"), ("DrawIndexedPrimitive", "vvvvvv", "A"),
    ("DrawPrimitiveUP", "vvvv", "X"), ("DrawIndexedPrimitiveUP", "vvvvvvvv", "X"), ("ProcessVertices", "vvvvvv", "S"),
    ("CreateVertexDeclaration", "vv", "L"), ("SetVertexDeclaration", "o", "X"), ("GetVertexDeclaration", "v", "S"),
    ("SetFVF", "v", "A"), ("GetFVF", "v", "S"), ("CreateVertexShader", "vv", "L"), ("SetVertexShader", "o", "A"),
    ("GetVertexShader", "v", "S"), ("SetVertexShaderConstantF", "vpc16@2v", "A"), ("GetVertexShaderConstantF", "vvv", "S"),
    ("SetVertexShaderConstantI", "vpc16@2v", "A"), ("GetVertexShaderConstantI", "vvv", "S"),
    ("SetVertexShaderConstantB", "vpc4@2v", "A"), ("GetVertexShaderConstantB", "vvv", "S"), ("SetStreamSource", "vovv", "X"),
    ("GetStreamSource", "vvvv", "S"), ("SetStreamSourceFreq", "vv", "A"), ("GetStreamSourceFreq", "vv", "S"),
    ("SetIndices", "o", "X"), ("GetIndices", "v", "S"), ("CreatePixelShader", "vv", "L"), ("SetPixelShader", "o", "A"),
    ("GetPixelShader", "v", "S"), ("SetPixelShaderConstantF", "vpc16@2v", "A"), ("GetPixelShaderConstantF", "vvv", "S"),
    ("SetPixelShaderConstantI", "vpc16@2v", "A"), ("GetPixelShaderConstantI", "vvv", "S"),
    ("SetPixelShaderConstantB", "vpc4@2v", "A"), ("GetPixelShaderConstantB", "vvv", "S"), ("DrawRectPatch", "vvv", "S"),
    ("DrawTriPatch", "vvv", "S"), ("DeletePatch", "v", "S"), ("CreateQuery", "vv", "L"),
]
FX = [
    ("QueryInterface", "vv", "L"), ("AddRef", "", "S"), ("Release", "", "X"), ("GetDesc", "v", "X"),
    ("GetParameterDesc", "vv", "X"), ("GetTechniqueDesc", "vv", "X"), ("GetPassDesc", "vv", "X"), ("GetFunctionDesc", "vv", "X"),
    ("GetParameter", "vv", "X"), ("GetParameterByName", "vv", "X"), ("GetParameterBySemantic", "vv", "X"),
    ("GetParameterElement", "vv", "X"), ("GetTechnique", "v", "X"), ("GetTechniqueByName", "v", "X"), ("GetPass", "vv", "X"),
    ("GetPassByName", "vv", "X"), ("GetFunction", "v", "X"), ("GetFunctionByName", "v", "X"), ("GetAnnotation", "vv", "X"),
    ("GetAnnotationByName", "vv", "X"), ("SetValue", "vvv", "X"), ("GetValue", "vvv", "S"), ("SetBool", "hv", "X"),
    ("GetBool", "vv", "S"), ("SetBoolArray", "hpc4@2v", "A"), ("GetBoolArray", "vvv", "S"), ("SetInt", "hv", "X"),
    ("GetInt", "vv", "X"), ("SetIntArray", "hpc4@2v", "A"), ("GetIntArray", "vvv", "S"), ("SetFloat", "hv", "X"),
    ("GetFloat", "vv", "S"), ("SetFloatArray", "hpc4@2v", "A"), ("GetFloatArray", "vvv", "S"), ("SetVector", "hp16", "X"),
    ("GetVector", "vv", "S"), ("SetVectorArray", "hpc16@2v", "A"), ("GetVectorArray", "vvv", "S"), ("SetMatrix", "hp64", "X"),
    ("GetMatrix", "vv", "S"), ("SetMatrixArray", "hpc64@2v", "A"), ("GetMatrixArray", "vvv", "S"),
    ("SetMatrixPointerArray", "vvv", "X"), ("GetMatrixPointerArray", "vvv", "S"), ("SetMatrixTranspose", "hp64", "X"),
    ("GetMatrixTranspose", "vv", "S"), ("SetMatrixTransposeArray", "hpc64@2v", "A"), ("GetMatrixTransposeArray", "vvv", "S"),
    ("SetMatrixTransposePointerArray", "vvv", "X"), ("GetMatrixTransposePointerArray", "vvv", "S"), ("SetString", "hs", "A"),
    ("GetString", "vv", "S"), ("SetTexture", "ho", "X"), ("GetTexture", "vv", "S"), ("GetPixelShader", "vv", "S"),
    ("GetVertexShader", "vv", "S"), ("SetArrayRange", "hvv", "A"), ("GetPool", "v", "L"), ("SetTechnique", "v", "X"),
    ("GetCurrentTechnique", "", "S"), ("ValidateTechnique", "v", "S"), ("FindNextValidTechnique", "vv", "S"),
    ("IsParameterUsed", "vv", "X"), ("Begin", "vv", "X"), ("BeginPass", "v", "A"), ("CommitChanges", "", "A"),
    ("EndPass", "", "A"), ("End", "", "A"), ("GetDevice", "v", "L"), ("OnLostDevice", "", "S"), ("OnResetDevice", "", "S"),
    ("SetStateManager", "v", "X"), ("GetStateManager", "v", "S"), ("BeginParameterBlock", "", "X"),
    ("EndParameterBlock", "", "X"), ("ApplyParameterBlock", "v", "X"), ("DeleteParameterBlock", "v", "X"),
    ("CloneEffect", "vv", "S"), ("SetRawValue", "hpc1@3vv", "X"),
]
RES8 = [("QueryInterface", "vv", "L"), ("AddRef", "", "D"), ("Release", "", "R"), ("GetDevice", "v", "D"),
        ("SetPrivateData", "vvvv", "S"), ("GetPrivateData", "vvv", "S"), ("FreePrivateData", "v", "S"),
        ("SetPriority", "v", "S"), ("GetPriority", "", "S"), ("PreLoad", "", "A"), ("GetType", "", "D")]
BASETEX = [("SetLOD", "v", "S"), ("GetLOD", "", "S"), ("GetLevelCount", "", "D"), ("SetAutoGenFilterType", "v", "S"),
           ("GetAutoGenFilterType", "", "S"), ("GenerateMipSubLevels", "", "A")]
TEX = RES8 + BASETEX + [("GetLevelDesc", "vv", "D"), ("GetSurfaceLevel", "vv", "D"), ("LockRect", "vvvv", "X"),
                        ("UnlockRect", "v", "X"), ("AddDirtyRect", "p16", "A")]
CUBE = RES8 + BASETEX + [("GetLevelDesc", "vv", "D"), ("GetCubeMapSurface", "vvv", "D"), ("LockRect", "vvvvv", "S"),
                         ("UnlockRect", "vv", "A"), ("AddDirtyRect", "vp16", "A")]
VTEX = RES8 + BASETEX + [("GetLevelDesc", "vv", "D"), ("GetVolumeLevel", "vv", "D"), ("LockBox", "vvvv", "S"),
                         ("UnlockBox", "v", "A"), ("AddDirtyBox", "p24", "A")]
SURF = RES8 + [("GetContainer", "vv", "D"), ("GetDesc", "v", "D"), ("LockRect", "vvv", "X"), ("UnlockRect", "", "X"),
               ("GetDC", "v", "S"), ("ReleaseDC", "v", "S")]
VOL = [("QueryInterface", "vv", "L"), ("AddRef", "", "D"), ("Release", "", "R"), ("GetDevice", "v", "D"),
       ("SetPrivateData", "vvvv", "S"), ("GetPrivateData", "vvv", "S"), ("FreePrivateData", "v", "S"),
       ("GetContainer", "vv", "D"), ("GetDesc", "v", "D"), ("LockBox", "vvv", "S"), ("UnlockBox", "", "A")]
VB = RES8 + [("Lock", "vvvv", "X"), ("Unlock", "", "X"), ("GetDesc", "v", "D")]
IB = VB
SB = [("QueryInterface", "vv", "L"), ("AddRef", "", "D"), ("Release", "", "R"), ("GetDevice", "v", "D"),
      ("Capture", "", "A"), ("Apply", "", "A")]
QRY = [("QueryInterface", "vv", "L"), ("AddRef", "", "D"), ("Release", "", "R"), ("GetDevice", "v", "D"),
       ("GetType", "", "D"), ("GetDataSize", "", "D"), ("Issue", "v", "A"), ("GetData", "vvv", "S")]
SC = [("QueryInterface", "vv", "L"), ("AddRef", "", "D"), ("Release", "", "R"), ("Present", "vvvvv", "X"),
      ("GetFrontBufferData", "v", "S"), ("GetBackBuffer", "vvv", "L"), ("GetRasterStatus", "v", "L"),
      ("GetDisplayMode", "v", "L"), ("GetDevice", "v", "D"), ("GetPresentParameters", "v", "L")]

SHADER = [("QueryInterface", "vv", "L"), ("AddRef", "", "D"), ("Release", "", "R"), ("GetDevice", "v", "D"), ("GetFunction", "vv", "S")]
DECL = [("QueryInterface", "vv", "L"), ("AddRef", "", "D"), ("Release", "", "R"), ("GetDevice", "v", "D"), ("GetDeclaration", "vv", "S")]
CLASSES = [("dev", DEV, 0), ("fx", FX, 120), ("tex", TEX, 200), ("cube", CUBE, 230), ("vtex", VTEX, 260),
           ("surf", SURF, 290), ("vol", VOL, 310), ("vb", VB, 330), ("ib", IB, 350), ("sb", SB, 370),
           ("q", QRY, 380), ("sc", SC, 390), ("vs", SHADER, 400), ("ps", SHADER, 410), ("decl", DECL, 420)]

def tokens(spec):
    out = []; i = 0
    while i < len(spec):
        c = spec[i]
        if c in "vohs":
            out.append(c); i += 1
        elif c == "p":
            j = i + 1
            if spec[j] == "c":
                k = j + 1
                while spec[k].isdigit(): k += 1
                mult = int(spec[j + 1:k]); assert spec[k] == "@"
                idx = int(spec[k + 1]); out.append(("pc", mult, idx)); i = k + 2
            else:
                k = j
                while k < len(spec) and spec[k].isdigit(): k += 1
                out.append(("p", int(spec[j:k]))); i = k
        else:
            raise ValueError(spec)
    return out

# statements that run first in a generated hook, on every path (device vertex-input shadow, see aotr_rt.inc)
PRE = {("dev", "SetFVF"): "rtDsInvalDecl();", ("dev", "DrawPrimitiveUP"): "rtDsInvalUP(0);", ("dev", "DrawIndexedPrimitiveUP"): "rtDsInvalUP(1);",
       ("dev", "BeginStateBlock"): "rtDsRecording(1);", ("dev", "EndStateBlock"): "rtDsRecording(0);", ("sb", "Apply"): "rtDsInvalAll();"}
lines = []
emit = lines.append
emit("// GENERATED by gen_rt_hooks.py - do not edit by hand.")
for cls, spec, base in CLASSES:
    emit("static void* g_rtO_%s[%d];" % (cls, len(spec)))
emit("")
names = {}
for cls, spec, base in CLASSES:
    for slot, (name, argspec, kind) in enumerate(spec):
        op = base + slot
        names[op] = "%s.%s" % (cls, name)
        toks = tokens(argspec)
        n = len(toks)
        params = "".join(", DWORD a%d" % i for i in range(n))
        pass_args = "".join(", a%d" % i for i in range(n))
        sig = "DWORD(__stdcall*)(void*%s)" % ("".join(", DWORD" for _ in range(n)))
        orig = "((%s)g_rtO_%s[%d])(self%s)" % (sig, cls, slot, pass_args)
        fname = "rt_%s_%d" % (cls, slot)
        if kind == "X":
            if cls == "dev" and name == "GetNPatchMode":
                emit("static float __stdcall %s(void* self);   // %s (hand-written, float return)" % (fname, name))
            else:
                emit("static DWORD __stdcall %s(void* self%s);   // %s (hand-written)" % (fname, params, name))
            continue
        emit("static DWORD __stdcall %s(void* self%s) {   // %s [%s %s]" % (fname, params, name, kind, argspec))
        if (cls, name) in PRE: emit("    " + PRE[(cls, name)])
        emit("    DWORD tid = __readfsdword(0x24);")
        if cls == "fx":
            emit("    if (!g_rtActive && !g_rtTried && g_mkTid && tid == g_mkTid && !g_rtXDepth) rtInstall(self);")
        emit("    if (tid != g_rtMainTid || !g_rtActive || g_rtDirectScope) {")
        emit("        if (g_rtActive && tid != g_rtWorkerTid && tid != g_rtMainTid) { EnterCriticalSection(&g_rtExecCs); DWORD r_ = %s; LeaveCriticalSection(&g_rtExecCs); InterlockedIncrement(&g_rtNForeign); return r_; }" % orig)
        emit("        return %s;" % orig)
        emit("    }")
        if kind == "S":
            emit("    rtSyncNote(%d, (DWORD)(ULONG_PTR)_ReturnAddress()); g_rtCurOp = %d;" % (op, op))
            emit("    rtDrain(); EnterCriticalSection(&g_rtExecCs); g_rtDirectScope++; DWORD r = %s; g_rtDirectScope--; LeaveCriticalSection(&g_rtExecCs);" % orig)
            emit("    g_rtNSync++; g_rtSyncHist[%d]++; return r;" % op)
        elif kind == "L":
            emit("    rtExecLock(); g_rtDirectScope++; DWORD r = %s; g_rtDirectScope--; LeaveCriticalSection(&g_rtExecCs);" % orig)
            emit("    g_rtNLocked++; return r;")
        elif kind == "D":
            emit("    return %s;" % orig)
        elif kind == "R":
            emit("    ULONG c = ((ULONG(__stdcall*)(void*))g_rtO_%s[1])(self);    // temporary reference keeps it alive until the queued releases run" % cls)
            # v27: a queued Release no longer marks the object as pending. It only drops a reference - it cannot change what a lock
            # reads or writes - and the game releases a level surface after every use, so the stamp made the NEXT lock of the same
            # glyph / staging surface wait for the whole queue (v26 log: "it is named by a queued (call without an op)").
            touch = ""
            emit("    for (int k = 0; k < 2; ++k) { RtRec* rr = rtAlloc(1, 0); rr->fn = g_rtO_%s[2]; rr->op = %d; rr->flags |= 2; rtArgs(rr)[0] = (DWORD)(ULONG_PTR)self;%s rtCommit(rr, 0); }" % (cls, op, touch))
            emit("    g_rtNRelease++; return c - 2;")
        elif kind == "A":
            # payload size
            size_terms = []
            for i, t in enumerate(toks):
                if isinstance(t, tuple) and t[0] == "p":
                    size_terms.append("(a%d ? %d : 0)" % (i, t[1]))
                elif isinstance(t, tuple) and t[0] == "pc":
                    size_terms.append("(a%d ? a%d * %d : 0)" % (i, t[2], t[1]))
                elif t == "h":
                    size_terms.append("rtHandleBytes(a%d)" % i)
                elif t == "s":
                    size_terms.append("rtStrBytes(a%d)" % i)
            emit("    DWORD pl = %s;" % (" + ".join(size_terms) if size_terms else "0"))
            emit("    RtRec* r = rtAlloc(%d, pl); r->fn = g_rtO_%s[%d]; r->op = %d;" % (n + 1, cls, slot, op))
            emit("    DWORD* A = rtArgs(r); BYTE* P = rtPayload(r); DWORD po = 0; (void)P; (void)po;")
            emit("    A[0] = (DWORD)(ULONG_PTR)self;")
            if cls in ('tex', 'cube', 'vtex', 'surf', 'vol'):
                emit("    rtTouch((DWORD)(ULONG_PTR)self);")
            for i, t in enumerate(toks):
                j = i + 1
                if t == "v":
                    emit("    A[%d] = a%d;" % (j, i))
                elif t == "o":
                    emit("    A[%d] = rtObj(r, %d, a%d);" % (j, j, i))
                elif t == "h":
                    emit("    A[%d] = rtHandle(P, &po, a%d);" % (j, i))
                elif t == "s":
                    emit("    A[%d] = rtStr(P, &po, a%d);" % (j, i))
                elif t[0] == "p":
                    emit("    A[%d] = rtBytes(P, &po, a%d, %d);" % (j, i, t[1]))
                elif t[0] == "pc":
                    emit("    A[%d] = rtBytes(P, &po, a%d, a%d ? a%d * %d : 0);" % (j, i, i, t[2], t[1]))
            emit("    rtCommit(r, po); return 0;")
        emit("}")
    emit("static void* const g_rtH_%s[%d] = {%s};" % (cls, len(spec), ", ".join("(void*)&rt_%s_%d" % (cls, s) for s in range(len(spec)))))
    emit("")
names[510] = "fx.batch"
names[509] = "d3dx.FilterTexture"
emit("static const char* rtOpName(int op) {")
emit("    switch (op) {")
for op in sorted(names):
    emit("    case %d: return \"%s\";" % (op, names[op]))
emit("    }")
emit("    return \"?\";")
emit("}")
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "aotr_rt_gen.inc")
open(out, "w", newline="\n").write("\n".join(lines) + "\n")
print("wrote %s: %d lines, %d hooked slots" % (out, len(lines), sum(len(s) for _, s, _ in CLASSES)))
