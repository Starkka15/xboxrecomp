/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState.
 *
 * ponytail: register-combiner state and texture stages 1-3 are not passed on:
 * a back end sees stage 0 and the diffuse colour. Extend Nv2aBatch when that
 * matters rather than adding callbacks.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>
#include "nv2a_combiner.h"

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
    /* SET_SURFACE_FORMAT word; the colour code is bits 0-3. X8R8G8B8_Z8R8G8B8 (4) and
     * X1R5G5B5_Z1R5G5B5 (1) store zero in the X bits whatever the pixel's
     * alpha; the _O variants (5, 2) store one. Titles read those bits back as
     * alpha when they sample the surface. */
    uint32_t color_format;
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
    uint32_t palette;           /* guest address of the CLUT, P8 only */
    uint32_t cube;              /* six faces, not one */
    uint32_t normalized;        /* coordinates are 0..1, not texels (swizzled, DXT) */
    /* BORDER (address mode 4): with border_color set, texels outside the image
     * read border (SET_TEXTURE_BORDER_COLOR, 0xAARRGGBB). Projected shadow maps
     * rely on it: a back end that samples black there darkens everything
     * outside the projection. Clear: the border comes from the image (clamp). */
    uint32_t border_color, border;
    /* SET_TEXTURE_CONTROL0 ALPHA_KILL_ENABLE: a pixel whose texel from this
     * stage has alpha 0 is not drawn at all (xemu: "if (tN.a == 0.0) discard").
     * Spyro ANB builds its shadow mask that way: a full-screen copy of the
     * frame keeps only the pixels its shadow volumes marked in alpha. */
    uint32_t alpha_kill;
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
    uint32_t specular;          /* 0xAARRGGBB, combiner register v1 */
    float    fog;               /* factor from the fog unit, combiner fog.a */
    float    tc[4][4];          /* each stage's coordinates as the title produced them */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
    /* SET_CLEAR_RECT_HORIZONTAL/VERTICAL: x0 | x1 << 16, y0 | y1 << 16, inclusive. */
    uint32_t clear_rect_h, clear_rect_v;
    uint32_t depth_bits;                                 /* 16 or 24 */
    /* SET_POLY_OFFSET_FILL_ENABLE (0x0338) and SET_POLYGON_OFFSET_SCALE_FACTOR/
     * BIAS (0x0384/0x0388), GL glPolygonOffset: factor * dz + units * r. Coplanar
     * decals (a page on a clipboard, eyes on a face) rely on it; without it they
     * z-fight with the surface under them whenever the camera moves. */
    uint32_t poly_offset_fill;
    float    poly_offset_factor, poly_offset_units;
    uint32_t control0;          /* SET_CONTROL0 (0x0290): Z_FORMAT float, Z_PERSPECTIVE (w-buffer) */
} Nv2aRenderState;

/* A batch whose vertex program the back end runs itself. verts holds nverts
 * vertices of slots float4s each, one slot per set bit of inputs from the
 * lowest up; attributes without a slot read imm[n]. indices is a triangle list.
 * aa is the anti-aliasing scale; near_z the screen z of the near plane. */
typedef struct {
    uint64_t hash;
    const float *verts;
    uint32_t nverts, slots, inputs;
    const uint32_t *indices;
    uint32_t nindices;
    const float (*constants)[4];    /* c[0..191] */
    float imm[16][4];
    float aa[2], near_z;
    float fog[4];                   /* param 0, param 1, 0 linear 1 exp 2 exp2, enable */
    uint32_t fog_abs;
} Nv2aProgramDraw;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;
    /* Everything the combiners read. stage[i] is NULL for an unset stage; rc is
     * NULL when the title has programmed no combiners. */
    const Nv2aTexture *stage[4];
    const Nv2aCombiner *rc;
    float    fog_color[3];          /* r, g, b */
    uint32_t clip_plane_mode;       /* SET_SHADER_CLIP_PLANE_MODE */
    const Nv2aProgramDraw *program; /* set: vertices is unused */
} Nv2aBatch;

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
    /* Optional. Make ready to run this program on the GPU, returning 0 to keep
     * it on the CPU path. A hash seen before answers at once. */
    int (*prepare_program)(uint64_t hash, const uint32_t (*ins)[4], uint32_t count, uint32_t inputs);
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Bytes the texture occupies in guest memory (level 0, one face), for content hashing. */
uint32_t nv2a_backend_texture_bytes(const Nv2aTexture *tex);

/* Bytes from one cube face to the next. */
uint32_t nv2a_backend_face_stride(const Nv2aTexture *tex);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#endif /* NV2A_BACKEND_H */
