// ---- wave 2 (2026-10-11): the GPU measurements gc3ds needs (docs/research/MEASURE_PLAN.md, tests G1-G18, M2) ----
// Included by gpu.c after its own tests, so it uses gpu.c's harness: measure() / frame_begin() / frame_end_timed()
// (bounded waits, g_dead after a timeout), center_drawn() (an undrawn target reports INVALID), and g_tgt.
// Not here on purpose: several DRAWARRAYS in one draw window (G5) and the geometry shader (G16) can hang the GPU;
// they get their own run.

typedef struct { float x, y, z; u8 r, g, b, a; float u, v; float idx; } VtxP;   // + palette offset (test G2/G3)
typedef struct { s16 x, y, z, w; u8 r, g, b, a; s16 u, v; } VtxI;                // integer attributes (G10)
_Static_assert(sizeof(VtxP) == 28, "VtxP layout");
_Static_assert(sizeof(VtxI) == 16, "VtxI layout");

enum { PX_PAD16, PX_PAD64, PX_PAD128, PX_PAL, PX_PAL3, PX_N };
static DVLB_s* g_dvlbX[PX_N];
static shaderProgram_s g_progX[PX_N];
static bool g_progXOk[PX_N];
static int g_uProjX[PX_N], g_uPal = -1, g_uPal3 = -1;
static VtxP* g_triP;
static VtxI* g_triI;
static Vtx* g_strip;          // 100 rows x 202 vertices, one triangle strip per row
static Vtx* g_w2q;            // 16 fog quads (6 each) + 1 combiner quad (6)
static void* g_vtxVram;       // copy of g_tri in VRAM (G9)
#define STRIP_ROW (2 * (GRID_W + 1))
#define FOG_DEPTHS 14
static const float kFogD[FOG_DEPTHS] = { 1.5f, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 80, 96 };
#define W2Q_COMB (FOG_DEPTHS * 6)

static int g_drawN, g_drawFirst;

static void w2_load_shaders(void)
{
    static const u8* bins[PX_N] = { gpu_pad16_shbin, gpu_pad64_shbin, gpu_pad128_shbin, gpu_pal_shbin, gpu_pal3_shbin };
    static const u32* sizes[PX_N] = { &gpu_pad16_shbin_size, &gpu_pad64_shbin_size, &gpu_pad128_shbin_size,
                                      &gpu_pal_shbin_size, &gpu_pal3_shbin_size };
    for (int i = 0; i < PX_N; i++) {
        g_progXOk[i] = load_shader(&g_dvlbX[i], &g_progX[i], bins[i], *sizes[i]);
        g_uProjX[i] = g_progXOk[i] ? shaderInstanceGetUniformLocation(g_progX[i].vertexShader, "projection") : -1;
        if (g_progXOk[i] && g_uProjX[i] < 0) { shaderProgramFree(&g_progX[i]); g_progXOk[i] = false; }
    }
    if (g_progXOk[PX_PAL]) g_uPal = shaderInstanceGetUniformLocation(g_progX[PX_PAL].vertexShader, "pal");
    if (g_uPal < 0 && g_progXOk[PX_PAL]) { shaderProgramFree(&g_progX[PX_PAL]); g_progXOk[PX_PAL] = false; }
    if (g_progXOk[PX_PAL3]) g_uPal3 = shaderInstanceGetUniformLocation(g_progX[PX_PAL3].vertexShader, "pal");
    if (g_uPal3 < 0 && g_progXOk[PX_PAL3]) { shaderProgramFree(&g_progX[PX_PAL3]); g_progXOk[PX_PAL3] = false; }
}

static void w2_free(void)
{
    if (g_triP) linearFree(g_triP);
    if (g_triI) linearFree(g_triI);
    if (g_strip) linearFree(g_strip);
    if (g_w2q) linearFree(g_w2q);
    if (g_vtxVram) vramFree(g_vtxVram);
    g_triP = NULL; g_triI = NULL; g_strip = NULL; g_w2q = NULL; g_vtxVram = NULL;
    for (int i = 0; i < PX_N; i++) {
        if (g_progXOk[i]) shaderProgramFree(&g_progX[i]);
        if (g_dvlbX[i]) DVLB_Free(g_dvlbX[i]);
        g_progXOk[i] = false;
        g_dvlbX[i] = NULL;
    }
}

static void use_x(int i, const C3D_Mtx* proj)
{
    C3D_BindProgram(&g_progX[i]);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uProjX[i], proj);
}

// Palette matrices 0..27 = identity rows (geometry unchanged), so a drawn target proves the indexing worked.
static void pal_identity(void)
{
    for (int m = 0; m < 28; m++)
        for (int r = 0; r < 3; r++)
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uPal + 3 * m + r, r == 0, r == 1, r == 2, 0.0f);
}

static void attr_vtxp(void)
{
    attr_vtx();
    AttrInfo_AddLoader(C3D_GetAttrInfo(), 3, GPU_FLOAT, 1);
}

static void w2_build(void)
{
    g_triP = linearAlloc(TRI_VERTS * sizeof(VtxP));
    g_triI = linearAlloc(TRI_VERTS * sizeof(VtxI));
    g_strip = linearAlloc(GRID_H * STRIP_ROW * sizeof(Vtx));
    g_w2q = linearAlloc((W2Q_COMB + 6) * sizeof(Vtx));
    if (g_tri && g_triP) {
        for (int k = 0; k < TRI_VERTS; k++) { memcpy(&g_triP[k], &g_tri[k], sizeof(Vtx)); g_triP[k].idx = 0.0f; }
        GSPGPU_FlushDataCache(g_triP, TRI_VERTS * sizeof(VtxP));
    }
    if (g_tri && g_triI) {
        for (int k = 0; k < TRI_VERTS; k++) {
            VtxI* o = &g_triI[k];
            o->x = (s16)lroundf(g_tri[k].x); o->y = (s16)lroundf(g_tri[k].y); o->z = 1; o->w = 1;
            o->r = g_tri[k].r; o->g = g_tri[k].g; o->b = g_tri[k].b; o->a = g_tri[k].a;
            o->u = (s16)(g_tri[k].u * 256); o->v = (s16)(g_tri[k].v * 256);
        }
        GSPGPU_FlushDataCache(g_triI, TRI_VERTS * sizeof(VtxI));
    }
    if (g_strip) {
        for (int j = 0; j < GRID_H; j++)
            for (int i = 0; i <= GRID_W; i++) {
                set_vtx(&g_strip[j * STRIP_ROW + 2 * i], i, j);
                set_vtx(&g_strip[j * STRIP_ROW + 2 * i + 1], i, j + 1);
            }
        GSPGPU_FlushDataCache(g_strip, GRID_H * STRIP_ROW * sizeof(Vtx));
    }
    if (g_w2q) {
        static const float c[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
        for (int q = 0; q < FOG_DEPTHS; q++)       // eye-space quads facing the camera at distance d, black
            for (int k = 0; k < 6; k++) {
                Vtx* v = &g_w2q[q * 6 + k];
                float d = kFogD[q];
                memset(v, 0, sizeof *v);
                v->x = c[k][0] * 2.0f * d; v->y = c[k][1] * 2.0f * d; v->z = -d; v->a = 255;
            }
        static const float px[6][2] = { { 0, 0 }, { SCR_W, 0 }, { SCR_W, SCR_H }, { 0, 0 }, { SCR_W, SCR_H }, { 0, SCR_H } };
        for (int k = 0; k < 6; k++) {              // full-screen quad, vertex colour 0x80 grey (combiner tests)
            Vtx* v = &g_w2q[W2Q_COMB + k];
            memset(v, 0, sizeof *v);
            v->x = px[k][0]; v->y = px[k][1]; v->z = 0.5f;
            v->r = v->g = v->b = 0x80; v->a = 0xFF;
        }
        GSPGPU_FlushDataCache(g_w2q, (W2Q_COMB + 6) * sizeof(Vtx));
    }
}

// Like measure(), and also the CPU time spent recording each frame's commands (draw()).
static double measure_cpu(DrawFn draw, int arg, double* cpuMs, bool* drawn)
{
    double total = 0, cpu = 0;
    int frames = 0, want = 3;
    *drawn = false;
    *cpuMs = -1;
    for (int i = 0;; i++) {
        if (!frame_begin()) return -1;
        C3D_FrameDrawOn(g_tgt);
        if (i == 0) C3D_RenderTargetClear(g_tgt, C3D_CLEAR_ALL, SENTINEL, 0);
        double c0 = now_s();
        draw(arg);
        double c = now_s() - c0;
        double t = frame_end_timed();
        if (t < 0) return -1;
        if (i == 0) continue;
        total += t;
        cpu += c;
        frames++;
        if (frames == 1) {
            want = t > 1e-6 ? (int)(MEASURE_S / t) : 200;
            if (want < 3) want = 3;
            if (want > 100) want = 100;
            if (g_quick) want = 3;
        }
        if (frames >= want || total > MEASURE_MAX_S) break;
    }
    *drawn = center_drawn(g_tgt);
    *cpuMs = cpu * 1000.0 / frames;
    return total * 1000.0 / frames;
}

// g_drawN draws of one triangle each from vertex g_drawFirst. raw: after the first (citro3d, which also commits
// state), each draw is the minimal register sequence citro3d's C3D_DrawArrays writes, minus PRIMITIVE_CONFIG and
// INDEXBUFFER_CONFIG (unchanged), in the same order: configuration before the draw window, never inside it.
static void draw_many(int raw)
{
    for (int i = 0; i < g_drawN; i++) {
        int first = g_drawFirst + 3 * i;
        if (!raw || i == 0) { C3D_DrawArrays(GPU_TRIANGLES, first, 3); continue; }
        GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
        GPUCMD_AddWrite(GPUREG_NUMVERTICES, 3);
        GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, (u32)first);
        GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 1);
        GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
        GPUCMD_AddWrite(GPUREG_DRAWARRAYS, 1);
        GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
        GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 0);
        GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);
    }
}

// g_drawN one-triangle draws, each after setting k palette uniforms (citro3d uploads dirty runs at the draw).
static void draw_unif(int k)
{
    for (int i = 0; i < g_drawN; i++) {
        for (int u = 0; u < k; u++) C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uPal + u, (u % 3) == 0, (u % 3) == 1, (u % 3) == 2, 0.0f);
        C3D_DrawArrays(GPU_TRIANGLES, g_drawFirst + 3 * i, 3);
    }
}

static void draw_strips(int unused)
{
    (void)unused;
    for (int j = 0; j < GRID_H; j++) C3D_DrawArrays(GPU_TRIANGLE_STRIP, j * STRIP_ROW, STRIP_ROW);
}

static void w2_tri_line(const char* name, int tris, double ms, bool drawn, double cpuMs)
{
    double m = ms > 0 ? tris / (ms * 1000.0) : -1;
    if (ms < 0) P("%-16s FAILED\n", name);
    else if (cpuMs >= 0) P("%-16s %6.2f ms/f %6.2f Mtri/s cpu %.2f ms%s\n", name, ms, m, cpuMs, drawn ? "" : " INVALID");
    else P("%-16s %6.2f ms/f %6.2f Mtri/s%s\n", name, ms, m, drawn ? "" : " INVALID");
    sum_add(name, drawn ? m : -1, "Mtri/s");
}

// G1: vertex-shader cost per instruction. G2: the matrix-palette shader.
static void test_w2_shaders(void)
{
    P("-- W2 G1/G2 vertex shader length + palette (60k tris) --\n");
    if (!g_tri || !g_triP) { P("G1 SKIPPED: alloc\n"); return; }
    static const char* nm[3] = { "vs pad16 60k", "vs pad64 60k", "vs pad128 60k" };
    bool drawn;
    step("w2 vs simple");
    if (!gpu_ok("w2 vs")) return;
    use_simple(&g_projTop); attr_vtx(); buf_vtx(g_tri); env_color();
    double base = measure(draw_tri_arrays, 3, true, &drawn);
    w2_tri_line("vs base 60k", 3 * GRID_TRIS, base, drawn, -1);
    for (int i = 0; i < 3; i++) {
        step(nm[i]);
        if (!gpu_ok(nm[i])) return;
        if (!g_progXOk[i]) { P("%s SKIPPED: shader\n", nm[i]); continue; }
        use_x(i, &g_projTop); attr_vtx(); buf_vtx(g_tri); env_color();
        double ms = measure(draw_tri_arrays, 3, true, &drawn);
        w2_tri_line(nm[i], 3 * GRID_TRIS, ms, drawn, -1);
        static const int pad[3] = { 16, 64, 128 };
        if (ms > 0 && base > 0) P("  +%d instr: %.2f ns/vertex per extra instruction\n", pad[i],
                                  (ms - base) * 1e6 / (3.0 * TRI_VERTS) / pad[i]);
    }
    if (!g_progXOk[PX_PAL]) { P("G2 SKIPPED: palette shader\n"); return; }
    static const int mats[4] = { 1, 8, 28, 0 };
    for (int s = 0; s < 4; s++) {
        char name[24];
        if (mats[s]) snprintf(name, sizeof name, "vs pal %dm 60k", mats[s]); else snprintf(name, sizeof name, "vs base 28B 60k");
        step(name);
        if (!gpu_ok(name)) return;
        int m = mats[s] ? mats[s] : 1;
        for (int k = 0; k < TRI_VERTS; k++) g_triP[k].idx = (float)(3 * ((k / 3) % m));
        GSPGPU_FlushDataCache(g_triP, TRI_VERTS * sizeof(VtxP));
        if (mats[s]) { use_x(PX_PAL, &g_projTop); pal_identity(); } else use_simple(&g_projTop);
        attr_vtxp();
        if (!buf_set(g_triP, sizeof(VtxP), 4, 0x3210)) { P("G2 SKIPPED: BufInfo\n"); return; }
        env_color();
        double ms = measure(draw_tri_arrays, 3, true, &drawn);
        w2_tri_line(strdup(name), 3 * GRID_TRIS, ms, drawn, -1);
    }
    show();
}

// G3: uniform upload cost. G4: CPU cost per draw call, citro3d vs the minimal register sequence.
static void test_w2_draws(void)
{
    P("-- W2 G3/G4 per-draw cost (one-triangle draws) --\n");
    if (!g_tri || !g_triP) { P("G3 SKIPPED: alloc\n"); return; }
    bool drawn;
    double cpu;
    g_drawFirst = (GRID_TRIS / 2 - 500) * 3;   // these triangles cover the target's centre
    for (int raw = 0; raw < 2; raw++) {
        const char* name = raw ? "draws raw 1000" : "draws c3d 1000";
        step(name);
        if (!gpu_ok(name)) return;
        use_simple(&g_projTop); attr_vtx(); buf_vtx(g_tri); env_color();
        g_drawN = 1000;
        double ms = measure_cpu(draw_many, raw, &cpu, &drawn);
        if (ms < 0) { P("%s FAILED\n", name); continue; }
        P("%-16s gpu %6.2f ms cpu %6.2f ms = %.2f us/draw cpu, %.2f us/draw gpu%s\n", name, ms, cpu,
          cpu * 1000.0 / g_drawN, ms * 1000.0 / g_drawN, drawn ? "" : " INVALID");
        sum_add(raw ? "draw cpu raw" : "draw cpu c3d", drawn ? cpu * 1000.0 / g_drawN : -1, "us");
    }
    if (!g_progXOk[PX_PAL]) { P("G3 SKIPPED: palette shader\n"); return; }
    static const int ks[5] = { 0, 3, 12, 48, 84 };
    for (int s = 0; s < 5; s++) {
        char name[24];
        snprintf(name, sizeof name, "unif %d vec4", ks[s]);
        step(name);
        if (!gpu_ok(name)) return;
        for (int k = 0; k < TRI_VERTS; k++) g_triP[k].idx = 0.0f;
        GSPGPU_FlushDataCache(g_triP, TRI_VERTS * sizeof(VtxP));
        use_x(PX_PAL, &g_projTop); pal_identity(); attr_vtxp();
        buf_set(g_triP, sizeof(VtxP), 4, 0x3210);
        env_color();
        // Keep the command list under ~150 KB: about (16 * k + 120) bytes per draw.
        g_drawN = 150000 / (16 * ks[s] + 120);
        if (g_drawN > 1000) g_drawN = 1000;
        g_drawFirst = (GRID_TRIS / 2 + GRID_W - g_drawN / 2) * 3;   // centred on the triangle at the target centre
        double ms = measure_cpu(draw_unif, ks[s], &cpu, &drawn);
        if (ms < 0) { P("%s FAILED\n", name); continue; }
        P("%-16s %4d draws: cpu %.2f us/draw, gpu %.2f us/draw%s\n", name, g_drawN, cpu * 1000.0 / g_drawN,
          ms * 1000.0 / g_drawN, drawn ? "" : " INVALID");
    }
}

// G6 strips, G9 vertices in VRAM, G10 integer attributes.
static void test_w2_vertex(void)
{
    P("-- W2 G6/G9/G10 primitive and vertex formats --\n");
    bool drawn;
    if (g_strip) {
        step("w2 strips");
        if (!gpu_ok("strips")) return;
        use_simple(&g_projTop); attr_vtx(); buf_vtx(g_strip); env_color();
        double ms = measure(draw_strips, 0, true, &drawn);
        w2_tri_line("strips 100x200", GRID_TRIS, ms, drawn, -1);
    }
    if (g_tri) {
        step("w2 list 20k");
        if (!gpu_ok("list")) return;
        use_simple(&g_projTop); attr_vtx(); buf_vtx(g_tri); env_color();
        double ms = measure(draw_tri_arrays, 1, true, &drawn);
        w2_tri_line("list 20k", GRID_TRIS, ms, drawn, -1);
    }
    if (g_tri) {
        step("w2 vram vertices");
        if (!gpu_ok("vram vtx")) return;
        u32 bytes = TRI_VERTS * sizeof(Vtx);
        g_vtxVram = vramAlloc(bytes);
        if (!g_vtxVram) P("vram vtx SKIPPED: vramAlloc\n");
        else if (frame_begin()) {
            GX_TextureCopy((u32*)g_tri, 0, (u32*)g_vtxVram, 0, bytes, COPY_RAW);
            if (frame_end_timed() >= 0) {
                use_simple(&g_projTop); attr_vtx();
                if (buf_vtx(g_vtxVram)) {
                    env_color();
                    double ms = measure(draw_tri_arrays, 3, true, &drawn);
                    w2_tri_line("vtx in VRAM 60k", 3 * GRID_TRIS, ms, drawn, -1);
                } else P("vram vtx SKIPPED: BufInfo\n");
            }
        }
    }
    if (g_triI) {
        step("w2 s16 vertices");
        if (!gpu_ok("s16 vtx")) return;
        C3D_Mtx m = g_projTop;
        Mtx_Scale(&m, 1.0f, 1.0f, 0.5f);           // integer z = 1 lands where the float tests put 0.5
        use_simple(&m);
        C3D_AttrInfo* ai = C3D_GetAttrInfo();
        AttrInfo_Init(ai);
        AttrInfo_AddLoader(ai, 0, GPU_SHORT, 4);
        AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
        AttrInfo_AddLoader(ai, 2, GPU_SHORT, 2);
        if (buf_set(g_triI, sizeof(VtxI), 3, 0x210)) {
            env_color();
            double ms = measure(draw_tri_arrays, 3, true, &drawn);
            w2_tri_line("vtx s16 16B 60k", 3 * GRID_TRIS, ms, drawn, -1);
        }
    }
    show();
}

// G12: fill rate by texture format (VRAM textures filled by a raw DMA copy of arbitrary bytes).
static void test_w2_texfmt(void)
{
    P("-- W2 G12 fill by texture format (VRAM) --\n");
    if (!g_quad || !g_stage) { P("G12 SKIPPED: alloc\n"); return; }
    static const struct { GPU_TEXCOLOR f; const char* name; } fm[] = {
        { GPU_RGBA8, "fill RGBA8" }, { GPU_RGB565, "fill RGB565" }, { GPU_RGBA4, "fill RGBA4" },
        { GPU_L8, "fill L8" }, { GPU_ETC1, "fill ETC1" },
    };
    for (unsigned i = 0; i < sizeof fm / sizeof fm[0]; i++) {
        step(fm[i].name);
        if (!gpu_ok(fm[i].name)) return;
        C3D_Tex t;
        if (!C3D_TexInitVRAM(&t, TEX_N, TEX_N, fm[i].f)) { P("%s SKIPPED: alloc\n", fm[i].name); continue; }
        bool ok = false;
        if (frame_begin()) {
            GX_TextureCopy(g_stage, 0, (u32*)t.data, 0, t.size, COPY_RAW);
            ok = frame_end_timed() >= 0;
        }
        if (ok) {
            C3D_TexSetFilter(&t, GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&t, GPU_REPEAT, GPU_REPEAT);
            use_simple(&g_projTop); attr_vtx(); buf_vtx(g_quad); env_tex(&t, 1);
            g_quadFirst = Q_FULL;
            bool drawn;
            double ms = measure(draw_quads, FILL_K, true, &drawn);
            report_fill(fm[i].name, ms, drawn);
        }
        if (!g_dead) C3D_TexDelete(&t);
    }
}

// G14: display-transfer / TextureCopy / fill speeds.
static double xfer_timed(int kind)
{
    double tot = 0;
    for (int r = 0; r < 5; r++) {
        if (!frame_begin()) return -1;
        u32* cb = (u32*)g_tgt->frameBuf.colorBuf;
        u32 dim = GX_BUFFER_DIM(SCR_H, SCR_W);
        switch (kind) {
        case 0: GX_DisplayTransfer(cb, dim, g_rb, dim, XFER_TO_LINEAR); break;
        case 1: GX_DisplayTransfer(cb, dim, g_rb, dim, (XFER_TO_LINEAR & ~GX_TRANSFER_OUT_FORMAT(7)) |
                                   GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565)); break;
        case 2: GX_DisplayTransfer(cb, dim, g_rb, GX_BUFFER_DIM(SCR_H / 2, SCR_W / 2),
                                   XFER_TO_LINEAR | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY)); break;
        case 3: GX_TextureCopy(g_stage, 0, (u32*)g_vtxVram, 0, TEX_BYTES, COPY_RAW); break;
        default: C3D_FrameDrawOn(g_tgt); C3D_RenderTargetClear(g_tgt, C3D_CLEAR_ALL, 0x10203040, 0); break;
        }
        double t = frame_end_timed();
        if (t < 0) return -1;
        tot += t;
    }
    return tot * 1000.0 / 5;
}

static void test_w2_transfer(void)
{
    P("-- W2 G14 DMA engine (incl. ~frame overhead) --\n");
    static const char* nm[5] = { "xfer 400x240 lin", "xfer ->RGB565", "xfer 2x2 down", "texcopy 256KB", "clear target" };
    static const double mb[5] = { SCR_PX * 4 / 1e6, SCR_PX * 4 / 1e6, SCR_PX * 4 / 1e6, TEX_BYTES / 1e6, SCR_PX * 8 / 1e6 };
    if (!g_rb || !g_stage) { P("G14 SKIPPED: alloc\n"); return; }
    if (!g_vtxVram) g_vtxVram = vramAlloc(TEX_BYTES);
    static const int order[5] = { 0, 1, 3, 4, 2 };   // the 2x2 downscale last: it hung the GPU in gcbench 4
    for (int oi = 0; oi < 5; oi++) {
        int k = order[oi];
        step(nm[k]);
        if (!gpu_ok(nm[k])) return;
        if (k == 3 && !g_vtxVram) { P("%s SKIPPED: no VRAM buffer\n", nm[k]); continue; }
        double ms = xfer_timed(k);
        if (ms < 0) { P("%s FAILED\n", nm[k]); continue; }
        P("%-16s %6.3f ms  %6.0f MB/s (source)\n", nm[k], ms, mb[k] / (ms / 1000.0));
        sum_add(nm[k], ms, "ms");
    }
}

// G7: what indexes the fog LUT. A linear LUT (f = index / 128) with black fragments and white fog makes the
// centre pixel (1 - index/128) * 255, for a camera-facing quad at each distance d (near 1, far 100).
static void test_w2_fog(void)
{
    P("-- W2 G7 fog index vs depth (near 1, far 100) --\n");
    if (!g_w2q) { P("G7 SKIPPED: alloc\n"); return; }
    C3D_Mtx proj;
    Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(60.0f), (float)SCR_W / SCR_H, 1.0f, 100.0f, false);
    float lutData[256];
    for (int i = 0; i < 128; i++) { lutData[i] = i / 128.0f; lutData[128 + i] = 1.0f / 128.0f; }
    C3D_FogLut lut;
    FogLut_FromArray(&lut, lutData);
    static const struct { bool zbuf; float scale, offset; bool flip; const char* name; } md[] = {
        { true, -1.0f, 0.0f, false, "z  s-1 o0" },
        { true, -1.0f, 0.0f, true, "z  s-1 o0 flip" },
        { false, -1.0f, 0.0f, false, "w  s-1 o0" },
        { false, -0.01f, 0.0f, false, "w  s-.01 o0" },
        { false, 0.0f, 0.01f, false, "w  s0 o.01" },
    };
    // Predictions from the projection matrix: clip z and w of (0, 0, -d, 1).
    P("d     ");
    for (int q = 0; q < FOG_DEPTHS; q++) P("%5.1f", kFogD[q]);
    P("\n-z/w  ");
    for (int q = 0; q < FOG_DEPTHS; q++) {
        float d = kFogD[q];
        float zc = proj.r[2].z * -d + proj.r[2].w, wc = proj.r[3].z * -d + proj.r[3].w;
        P("%5.2f", wc != 0 ? -zc / wc : 0);
    }
    P("\n");
    for (unsigned m = 0; m < sizeof md / sizeof md[0]; m++) {
        step(md[m].name);
        if (!gpu_ok("fog")) break;
        int idx[FOG_DEPTHS];
        bool ok = true;
        for (int q = 0; q < FOG_DEPTHS && ok; q++) {
            if (!frame_begin()) { ok = false; break; }
            C3D_FrameDrawOn(g_tgt);
            C3D_RenderTargetClear(g_tgt, C3D_CLEAR_ALL, SENTINEL, 0);
            use_simple(&proj); attr_vtx(); buf_vtx(g_w2q); env_color(); depth_off();
            C3D_DepthMap(md[m].zbuf, md[m].scale, md[m].offset);
            C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, md[m].flip);
            C3D_FogColor(0xFFFFFFFF);
            C3D_FogLutBind(&lut);
            C3D_DrawArrays(GPU_TRIANGLES, q * 6, 6);
            if (frame_end_timed() < 0 || !read_target(g_tgt)) { ok = false; break; }
            u32 px = g_rb[(SCR_W / 2) * SCR_H + SCR_H / 2];
            int v = (int)((px >> 8) & 0xFF);
            idx[q] = px == SENTINEL ? -1 : (int)lroundf(128.0f * (1.0f - v / 255.0f));
        }
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        C3D_DepthMap(true, -1.0f, 0.0f);
        if (!ok) { P("%-12s FAILED\n", md[m].name); break; }
        P("%-12s", md[m].name);
        for (int q = 0; q < FOG_DEPTHS; q++) P("%5d", idx[q]);
        P("\n");
    }
    P("  (row = fog LUT index 0..128 per distance; -1 = not drawn)\n");
}

// G18: exact combiner arithmetic. Stage 0 = constant A; stage 1 = op(previous A, constant B, vertex colour 0x80).
static void test_w2_combiner(void)
{
    P("-- W2 G18 combiner rounding (8-bit, grey) --\n");
    if (!g_w2q) { P("G18 SKIPPED: alloc\n"); return; }
    static const struct { GPU_COMBINEFUNC f; u8 a, b; int scale; int expect; const char* name; } cs[] = {
        { GPU_MODULATE, 0x80, 0x80, 1, 0x40, "mod 80*80" },
        { GPU_MODULATE, 0xFF, 0x7F, 1, 0x7F, "mod FF*7F" },
        { GPU_MODULATE, 0x80, 0x80, 2, 0x80, "mod 80*80 x2" },
        { GPU_ADD, 0x90, 0x90, 1, 0xFF, "add sat" },
        { GPU_ADD_SIGNED, 0x80, 0x80, 1, 0x80, "addsig 80+80" },
        { GPU_ADD_SIGNED, 0xC0, 0x50, 1, 0x90, "addsig C0+50" },
        { GPU_ADD_SIGNED, 0x40, 0x30, 1, 0x00, "addsig 40+30" },
        { GPU_SUBTRACT, 0x80, 0x30, 1, 0x50, "sub 80-30" },
        { GPU_SUBTRACT, 0x30, 0x80, 1, 0x00, "sub 30-80" },
        { GPU_INTERPOLATE, 0xFF, 0x00, 1, 0x80, "lerp FF,00 @80" },
        { GPU_MULTIPLY_ADD, 0x80, 0x80, 1, 0xC0, "mad 80*80+80" },
        { GPU_ADD_MULTIPLY, 0x40, 0x40, 1, 0x40, "(40+40)*80" },
    };
    for (unsigned i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        step("w2 combiner");
        if (!gpu_ok("combiner")) return;
        if (!frame_begin()) return;
        C3D_FrameDrawOn(g_tgt);
        C3D_RenderTargetClear(g_tgt, C3D_CLEAR_ALL, SENTINEL, 0);
        use_simple(&g_projTop); attr_vtx(); buf_vtx(g_w2q); depth_off(); blend_off();
        C3D_TexBind(0, NULL);
        C3D_TexEnv* e = C3D_GetTexEnv(0);
        C3D_TexEnvInit(e);
        C3D_TexEnvSrc(e, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(e, 0xFF000000u | cs[i].a * 0x010101u);
        e = C3D_GetTexEnv(1);
        C3D_TexEnvInit(e);
        C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(e, C3D_RGB, cs[i].f);
        C3D_TexEnvColor(e, 0xFF000000u | cs[i].b * 0x010101u);
        if (cs[i].scale == 2) C3D_TexEnvScale(e, C3D_RGB, GPU_TEVSCALE_2);
        env_passthrough_from(2);
        C3D_DrawArrays(GPU_TRIANGLES, W2Q_COMB, 6);
        if (frame_end_timed() < 0 || !read_target(g_tgt)) return;
        u32 px = g_rb[(SCR_W / 2) * SCR_H + SCR_H / 2];
        int v = (int)((px >> 8) & 0xFF);
        P("%-15s got %02X expect %02X%s\n", cs[i].name, v, cs[i].expect, v == cs[i].expect ? "" : " <-");
    }
    env_color();
}

// M2: CPU memcpy bandwidth while the GPU fills (contention on FCRAM).
static void test_w2_contention(void)
{
    P("-- W2 M2 memcpy while the GPU fills --\n");
    step("w2 contention");
    if (!gpu_ok("contention") || !g_quad || !g_texVOk) return;
    const u32 n = 2u << 20;
    u8 *a = malloc(n), *b = malloc(n);
    if (!a || !b) { P("M2 SKIPPED: malloc\n"); free(a); free(b); return; }
    memset(a, 1, n); memset(b, 2, n);
    double t0 = now_s();
    int copies = 0;
    while (now_s() - t0 < 0.05) { memcpy(b, a, n); copies++; }
    double idle = copies * (n / 1e6) / (now_s() - t0);
    if (!frame_begin()) { free(a); free(b); return; }
    C3D_FrameDrawOn(g_tgt);
    use_simple(&g_projTop); attr_vtx(); buf_vtx(g_quad); env_tex(&g_texV, 1);
    g_quadFirst = Q_FULL;
    draw_quads(96);                                  // ~48 ms of fill at 193 Mpx/s
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    t0 = now_s();
    copies = 0;
    while (now_s() - t0 < 0.03) { memcpy(b, a, n); copies++; }
    double tc = now_s() - t0, busy = copies * (n / 1e6) / tc;
    bool stillBusy = !C3D_FrameBegin(C3D_FRAME_NONBLOCK);
    if (!stillBusy) C3D_FrameEnd(GX_CMDLIST_FLUSH);
    double w0 = now_s();
    while (stillBusy) {
        if (C3D_FrameBegin(C3D_FRAME_NONBLOCK)) { C3D_FrameEnd(GX_CMDLIST_FLUSH); break; }
        if (now_s() - w0 > GPU_TIMEOUT_S) { gpu_timeout(); break; }
        svcSleepThread(POLL_NS);
    }
    P("memcpy idle %.0f MB/s, while GPU fills %.0f MB/s (%.0f%%)%s\n", idle, busy, 100.0 * busy / idle,
      stillBusy ? "" : " (GPU finished early: overlap partial)");
    sum_add("memcpy gpu busy", busy, "MB/s");
    free(a);
    free(b);
}

// G2b: what an extra vertex attribute costs (gcbench 4: a 4th attribute took 60k triangles from 6.0 to 3.6 Mtri/s
// while 128 extra shader instructions cost nothing). Same triangles, 2 / 3 / 4 attributes, and the palette with its
// index packed into position.w (3 attributes) against the 4-attribute palette.
typedef struct { float x, y, z, idx; u8 r, g, b, a; float u, v; } VtxQ;   // 28 bytes, 3 attributes
typedef struct { float x, y, z; u8 r, g, b, a; } VtxC;                   // 16 bytes, 2 attributes
_Static_assert(sizeof(VtxQ) == 28, "VtxQ layout");
_Static_assert(sizeof(VtxC) == 16, "VtxC layout");

static void test_w2_attrs(void)
{
    P("-- W2 G2b attributes vs palette index packing (60k tris) --\n");
    if (!g_tri || !g_triP) { P("G2b SKIPPED: alloc\n"); return; }
    VtxQ* q = linearAlloc(TRI_VERTS * sizeof(VtxQ));
    VtxC* c = linearAlloc(TRI_VERTS * sizeof(VtxC));
    if (!q || !c) { P("G2b SKIPPED: alloc\n"); if (q) linearFree(q); if (c) linearFree(c); return; }
    for (int k = 0; k < TRI_VERTS; k++) {
        const Vtx* v = &g_tri[k];
        q[k] = (VtxQ){ v->x, v->y, v->z, (float)(3 * ((k / 3) % 28)), v->r, v->g, v->b, v->a, v->u, v->v };
        c[k] = (VtxC){ v->x, v->y, v->z, v->r, v->g, v->b, v->a };
        g_triP[k].idx = (float)(3 * ((k / 3) % 28));
    }
    GSPGPU_FlushDataCache(q, TRI_VERTS * sizeof(VtxQ));
    GSPGPU_FlushDataCache(c, TRI_VERTS * sizeof(VtxC));
    GSPGPU_FlushDataCache(g_triP, TRI_VERTS * sizeof(VtxP));
    bool drawn;
    for (int t = 0; t < 5; t++) {
        static const char* nm[5] = { "attr2 16B", "attr3 24B", "attr4 28B", "pal 4attr 28m", "pal3 packed 28m" };
        step(nm[t]);
        if (!gpu_ok(nm[t])) break;
        C3D_AttrInfo* ai = C3D_GetAttrInfo();
        AttrInfo_Init(ai);
        bool ok = true;
        switch (t) {
        case 0:
            use_simple(&g_projTop);
            AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
            AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
            ok = buf_set(c, sizeof(VtxC), 2, 0x10);
            break;
        case 1:
            use_simple(&g_projTop); attr_vtx(); ok = buf_vtx(g_tri);
            break;
        case 2:
        case 3:
            if (t == 3 && !g_progXOk[PX_PAL]) { ok = false; break; }
            if (t == 3) { use_x(PX_PAL, &g_projTop); pal_identity(); } else use_simple(&g_projTop);
            attr_vtxp();
            ok = buf_set(g_triP, sizeof(VtxP), 4, 0x3210);
            break;
        default:
            if (!g_progXOk[PX_PAL3]) { ok = false; break; }
            use_x(PX_PAL3, &g_projTop);
            for (int m = 0; m < 28; m++)
                for (int r = 0; r < 3; r++)
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uPal3 + 3 * m + r, r == 0, r == 1, r == 2, 0.0f);
            AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
            AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
            AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
            ok = buf_set(q, sizeof(VtxQ), 3, 0x210);
            break;
        }
        if (!ok) { P("%s SKIPPED: shader or BufInfo\n", nm[t]); continue; }
        env_color();
        double ms = measure(draw_tri_arrays, 3, true, &drawn);
        w2_tri_line(nm[t], 3 * GRID_TRIS, ms, drawn, -1);
    }
    if (!g_dead) { linearFree(q); linearFree(c); }
}

// M6 again, done right: each core's counter loop checks the clock only every 64k increments.
static volatile int g_ctrStop;
static void ctr_worker(void* arg)
{
    volatile u32* p = arg;
    while (!g_ctrStop) for (int i = 0; i < 65536; i++) (*p)++;
}

static void test_w2_falseshare(void)
{
    P("-- W2 M6b counters on one cache line vs separate lines --\n");
    static volatile u32 ctrs[16] __attribute__((aligned(32)));
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    for (int far = 0; far < 2; far++) {
        step("false sharing");
        memset((void*)ctrs, 0, sizeof ctrs);
        g_ctrStop = 0;
        volatile u32* other = &ctrs[far ? 8 : 1];
        Thread th = threadCreate(ctr_worker, (void*)other, 8 * 1024, prio, 2, false);
        if (!th) { P("M6b SKIPPED: thread\n"); return; }
        double t0 = now_s();
        while (now_s() - t0 < 0.2) for (int i = 0; i < 65536; i++) ctrs[0]++;
        double t = now_s() - t0;
        g_ctrStop = 1;
        threadJoin(th, U64_MAX);
        threadFree(th);
        P("%s: core0 %.1f M/s, core2 %.1f M/s\n", far ? "separate lines" : "same line   ", ctrs[0] / t / 1e6, *other / t / 1e6);
    }
}

static void bench_gpu_w2(void)
{
    if (g_followup) {
        P("\n== GPU follow-up (gcbench 4b) ==\n");
        step("w2 setup");
        w2_load_shaders();
        w2_build();
        test_w2_attrs();
        test_w2_combiner();
        test_w2_contention();
        test_w2_falseshare();
        test_w2_transfer();   // last: its 2x2 downscale hung the GPU in gcbench 4
        return;
    }
    P("\n== GPU wave 2 (gc3ds plan) ==\n");
    step("w2 setup");
    w2_load_shaders();
    w2_build();
    P("w2 shaders: pad16 %d pad64 %d pad128 %d pal %d (uPal %d)\n", g_progXOk[0], g_progXOk[1], g_progXOk[2],
      g_progXOk[3], g_uPal);
    test_w2_fog();
    test_w2_shaders();
    test_w2_draws();
    test_w2_vertex();
    test_w2_texfmt();
    test_w2_attrs();
    test_w2_combiner();
    test_w2_contention();
    test_w2_falseshare();
    test_w2_transfer();   // last: its 2x2 downscale hung the GPU in gcbench 4
}
