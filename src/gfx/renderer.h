#ifndef PDO_RENDERER_H
#define PDO_RENDERER_H

/*
 * The host side of our Direct3D replacement, on the SDL3 GPU API.
 *
 * Each frame is recorded as a list of commands (clears and draws, with all
 * the state they need copied in) and replayed when the game calls Swap:
 * vertices, indices and textures are uploaded once, then every command is
 * drawn in order. Recording rather than drawing immediately is what later
 * allows interpolated frames and comparing frames against captures; see
 * docs/design/widescreen-and-framerate.md.
 *
 * The game renders into an internal target at its own resolution, presented
 * scaled with its aspect ratio kept. Debug switches (environment):
 *   PDO_SCREENSHOT=dir      save presented frames as BMP (first 300 saved)
 *   PDO_SCREENSHOT_EVERY=n  only every n-th frame
 */

#include <stdbool.h>
#include <stdint.h>

bool pdo_gfx_init(int width, int height);
void pdo_gfx_shutdown(void);

/* One vertex as the shaders see it. Positions are in clip space; colours are
 * D3DCOLORs; four texture coordinate sets of up to four components; fog is
 * the fog coordinate (eye distance, or the fog factor itself when the fog
 * table mode is NONE). psize is the point size in pixels, for point lists
 * (the shaders do not read it). */
typedef struct pdo_vertex {
    float    pos[4];
    uint32_t diffuse, specular;
    float    tc[4][4];
    float    fog;
    float    psize;
} pdo_vertex;

/* A texture as the game described it: a guest D3DPixelContainer's data
 * address and Format/Size words, plus how it is sampled (XDK D3DTEXF_* and
 * D3DTADDRESS_* values). data == 0 means no texture. */
typedef struct pdo_tex_ref {
    uint32_t data, format_word, size_word;
    uint8_t  min_filter, mag_filter, mip_filter;
    uint8_t  address_u, address_v;
} pdo_tex_ref;

/* Fixed-function stage setup, laid out as the fragment shader's std140
 * uniform block (src/gfx/shaders/ff.frag). */
typedef struct pdo_ff_uniforms {
    int32_t color_op[4][4];     /* op, arg1, arg2, arg0 */
    int32_t alpha_op[4][4];
    int32_t stage_misc[4][4];   /* result arg, has texture, texcoord set, - */
    float   tfactor[4];
    int32_t alpha_test[4];      /* enable, func, ref, specular enable */
    /* A pixel shader (NV2A register combiners), straight from the XDK's
     * D3DPIXELSHADERDEF. ps_final: final combiner ABCD inputs, EFG inputs,
     * PSCombinerCount (0 = no pixel shader: the stages above apply),
     * PSTextureModes. */
    int32_t ps_rgb_in[8], ps_rgb_out[8], ps_a_in[8], ps_a_out[8];
    float   ps_c0[8][4], ps_c1[8][4];
    int32_t ps_final[4];
    float   ps_fc0[4], ps_fc1[4];
    /* Per stage: texture coordinate scale (x, y). Linear (non-swizzled)
     * textures are addressed in texels on the NV2A, so 1/width, 1/height;
     * swizzled and compressed ones are normalised already, so 1.
     * z: 1 if the format has no alpha (alpha reads as 1).
     * w: 1 if red and green are signed (V8U8), stored offset by 128. */
    float   tc_scale[4][4];
    /* Fog: enable, D3DFOGMODE (0 NONE: the coordinate is the factor,
     * 1 EXP, 2 EXP2, 3 LINEAR); colour; start, end, density. */
    int32_t fog_mode[4];
    float   fog_color[4];
    float   fog_params[4];
    /* Per stage, for texture mode BUMPENVMAP(_LUM): the bump matrix in
     * texture stage state order (00, 01, 11, 10) and luminance scale,
     * offset. */
    float   bump_mat[4][4];
    float   bump_lum[4][4];
} pdo_ff_uniforms;

/* Fixed-function output state, in the XDK's GL-valued enums. */
typedef struct pdo_pipeline_state {
    uint8_t  topology;          /* 0 triangles, 1 lines, 2 points */
    uint8_t  blend_enable;
    uint16_t src_blend, dst_blend, blend_op;
    uint8_t  z_enable, z_write;
    uint8_t  cull;              /* 0 none, 1 cull clockwise, 2 counter-clockwise */
    uint16_t z_func;
    uint32_t color_write;       /* Xbox D3DCOLORWRITEENABLE_* bits */
    /* Polygon offset (D3DRS_ZBIAS sets these): constant in depth-buffer
     * units, and slope scale. Decals rely on it to win against the wall. */
    float    depth_bias, depth_slope;
    /* Stencil (GL-valued, as the XDK stores them): enable, func, ops for
     * stencil fail / depth fail / pass, read and write masks. */
    uint8_t  stencil_enable, stencil_mask, stencil_write_mask;
    uint16_t stencil_func, stencil_fail, stencil_zfail, stencil_pass;
} pdo_pipeline_state;

typedef struct pdo_draw {
    pdo_pipeline_state ps;
    pdo_tex_ref        tex[4];
    pdo_ff_uniforms    ff;
    float              viewport[6];  /* x, y, w, h, min z, max z */
    uint8_t            stencil_ref;
    /* For captures only: the vertex shader (FVF or program handle),
     * lighting enable, COLORVERTEX with the lights switched on (a bit each)
 * in bits 8-15, material sources (diffuse, ambient,
     * specular, emissive: a nibble each), AMBIENT, the material's diffuse,
     * ambient and emissive as D3DCOLORs. */
    uint32_t           vertex_info[8];
} pdo_draw;

/* Record a draw. idx are indices into v; both are copied. */
void pdo_gfx_draw(const pdo_draw *d, const pdo_vertex *v, uint32_t nv,
                  const uint32_t *idx, uint32_t ni);

/* D3DCLEAR_* flags: TARGET 0xF0, ZBUFFER 0x01, STENCIL 0x02. color is a
 * D3DCOLOR (ARGB). */
void pdo_gfx_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil);

/* A guest surface: a D3DPixelContainer's data address and Format/Size
 * words. data == 0 is the back buffer (the frame we present). */
typedef struct pdo_surface {
    uint32_t data, format_word, size_word;
} pdo_surface;

/* D3DDevice_SetRenderTarget: later clears and draws go to this surface. A
 * texture whose data address is a surface drawn to this way samples what was
 * drawn (the GPU copy), not guest memory. */
void pdo_gfx_set_target(const pdo_surface *color);

/* D3DDevice_CopyRects, one rectangle: src (x, y, w, h) to dst at (dx, dy),
 * clipped to both surfaces. Either may be the back buffer. */
void pdo_gfx_copy_rect(const pdo_surface *src, const pdo_surface *dst,
                       int x, int y, int w, int h, int dx, int dy);

/* Width and height of a surface (the frame's for the back buffer). */
void pdo_gfx_surface_size(const pdo_surface *s, int *w, int *h);

/* The display gamma ramp: 256 red, then 256 green, then 256 blue entries
 * (Xbox D3DGAMMARAMP), applied when the frame is presented. */
void pdo_gfx_set_gamma(const uint8_t ramp[768]);

/* Replay the recorded frame and show it. Returns false if the player closed
 * the window. */
bool pdo_gfx_present(void);
/* Window events between frames (loading, movies): false when the window was
 * closed. */
bool pdo_gfx_pump_events(void);

uint64_t pdo_gfx_frame_count(void);
void pdo_gfx_target_size(int *w, int *h);

#endif
