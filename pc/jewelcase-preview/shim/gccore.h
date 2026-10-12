/* Host shim: the slice of libogc2 that jewelcase.c and scene3d.h use, rendered in software */
#ifndef SHIM_GCCORE_H
#define SHIM_GCCORE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef float f32; typedef volatile u32 vu32;
typedef f32 Mtx[3][4]; typedef f32 (*MtxP)[4]; typedef f32 Mtx44[4][4];
typedef struct { u8 r, g, b, a; } GXColor;
typedef struct { const void *data; u16 w, h; u8 fmt, wrapS, wrapT, mip; int minFilt, magFilt; float minLod, maxLod; } GXTexObj;
typedef struct { u32 v[3]; } GXTlutObj;
typedef u32 mutex_t; typedef u32 lwp_t;
#define LWP_MUTEX_NULL 0xffffffff
#define LWP_THREAD_NULL 0xffffffff
#define ATTRIBUTE_ALIGN(x) __attribute__((aligned(x)))
static inline s32 LWP_MutexInit(mutex_t *m, bool r) { (void)r; *m = 1; return 0; }
static inline s32 LWP_MutexLock(mutex_t m) { (void)m; return 0; }
static inline s32 LWP_MutexUnlock(mutex_t m) { (void)m; return 0; }
static inline void DCFlushRange(void *p, u32 n) { (void)p; (void)n; }
static inline void DCInvalidateRange(void *p, u32 n) { (void)p; (void)n; }
typedef struct { char gamename[4]; char company[2]; u8 disknum, gamever, streaming, streambufsize; u8 pad[22]; } dvddiskid;

enum { GX_FALSE = 0, GX_TRUE = 1, GX_DISABLE = 0, GX_ENABLE = 1 };
#define GX_QUADS 0x80
#define GX_TRIANGLES 0x90
#define GX_TRIANGLESTRIP 0x98
#define GX_TRIANGLEFAN 0xA0
#define GX_LINES 0xA8
#define GX_LINESTRIP 0xB0
#define GX_POINTS 0xB8
enum { GX_VTXFMT0, GX_VTXFMT1, GX_VTXFMT2, GX_VTXFMT3, GX_VTXFMT4, GX_VTXFMT5, GX_VTXFMT6, GX_VTXFMT7 };
enum { GX_VA_PNMTXIDX = 0, GX_VA_TEX0MTXIDX = 1, GX_VA_POS = 9, GX_VA_NRM = 10, GX_VA_CLR0 = 11, GX_VA_CLR1 = 12, GX_VA_TEX0 = 13 };
enum { GX_NONE = 0, GX_DIRECT = 1, GX_INDEX8 = 2, GX_INDEX16 = 3 };
enum { GX_POS_XYZ = 1, GX_NRM_XYZ = 0, GX_CLR_RGBA = 1, GX_TEX_ST = 1 };
enum { GX_F32 = 4, GX_RGBA8 = 5, GX_S16 = 3 };
enum { GX_PERSPECTIVE = 0, GX_ORTHOGRAPHIC = 1 };
enum { GX_PNMTX0 = 0 };
#define GX_TEXMTX0 30
#define GX_TEXMTX1 33
#define GX_TEXMTX2 36
#define GX_IDENTITY 60
enum { GX_MTX2x4 = 0, GX_MTX3x4 = 1 };
enum { GX_TG_MTX3x4 = 0, GX_TG_MTX2x4 = 1 };
enum { GX_TG_POS = 0, GX_TG_NRM = 1, GX_TG_TEX0 = 4 };
enum { GX_TEXCOORD0 = 0, GX_TEXCOORD1 = 1, GX_TEXCOORDNULL = 0xff };
enum { GX_TEXMAP0 = 0, GX_TEXMAP1 = 1, GX_TEXMAP_NULL = 0xff };
enum { GX_COLOR0 = 0, GX_ALPHA0 = 2, GX_COLOR0A0 = 4, GX_COLORNULL = 0xff };
enum { GX_SRC_REG = 0, GX_SRC_VTX = 1 };
#define GX_LIGHTNULL 0
enum { GX_DF_NONE = 0, GX_DF_CLAMP = 2 };
enum { GX_AF_NONE = 2 };
enum { GX_TEVSTAGE0 = 0, GX_TEVSTAGE1 = 1, GX_TEVSTAGE2 = 2 };
enum { GX_MODULATE = 0, GX_DECAL, GX_BLEND, GX_REPLACE, GX_PASSCLR };
enum { GX_CC_CPREV = 0, GX_CC_APREV, GX_CC_C0, GX_CC_A0, GX_CC_C1, GX_CC_A1, GX_CC_C2, GX_CC_A2, GX_CC_TEXC, GX_CC_TEXA, GX_CC_RASC, GX_CC_RASA, GX_CC_ONE, GX_CC_HALF, GX_CC_KONST, GX_CC_ZERO };
enum { GX_CA_APREV = 0, GX_CA_A0, GX_CA_A1, GX_CA_A2, GX_CA_TEXA, GX_CA_RASA, GX_CA_KONST, GX_CA_ZERO };
enum { GX_TEV_ADD = 0, GX_TEV_SUB = 1 };
enum { GX_TB_ZERO = 0 };
enum { GX_CS_SCALE_1 = 0, GX_CS_SCALE_2 = 1, GX_CS_SCALE_4 = 2, GX_CS_DIVIDE_2 = 3 };
enum { GX_TEVPREV = 0, GX_TEVREG0 = 1, GX_TEVREG1 = 2, GX_TEVREG2 = 3 };
enum { GX_KCOLOR0 = 0, GX_KCOLOR1, GX_KCOLOR2, GX_KCOLOR3 };
enum { GX_TEV_KCSEL_1 = 0, GX_TEV_KCSEL_K0 = 12 };
enum { GX_TEV_KASEL_1 = 0, GX_TEV_KASEL_K0_A = 28 };
enum { GX_BM_NONE = 0, GX_BM_BLEND = 1 };
enum { GX_BL_ZERO = 0, GX_BL_ONE, GX_BL_SRCCLR, GX_BL_INVSRCCLR, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA };
enum { GX_LO_CLEAR = 0 };
enum { GX_NEVER = 0, GX_LESS, GX_EQUAL, GX_LEQUAL, GX_GREATER, GX_NEQUAL, GX_GEQUAL, GX_ALWAYS };
enum { GX_AOP_AND = 0 };
enum { GX_CULL_NONE = 0 };
enum { GX_CLAMP = 0, GX_REPEAT = 1 };
enum { GX_TF_I8 = 1, GX_TF_RGBA8 = 6 };
enum { GX_NEAR = 0, GX_LINEAR, GX_NEAR_MIP_NEAR, GX_LIN_MIP_NEAR, GX_NEAR_MIP_LIN, GX_LIN_MIP_LIN };
enum { GX_ANISO_1 = 0 };
enum { GX_TO_ZERO = 0 };

void GX_ClearVtxDesc(void); void GX_SetVtxDesc(u8 attr, u8 type); void GX_SetVtxAttrFmt(u8 fmt, u32 attr, u32 cnt, u32 type, u8 frac);
void GX_SetArray(u32 attr, const void *ptr, u8 stride);
void GX_Begin(u8 prim, u8 fmt, u16 n); void GX_End(void);
void GX_Position3f32(f32 x, f32 y, f32 z); void GX_Position1x16(u16 i); void GX_Normal1x16(u16 i);
void GX_Normal3f32(f32 x, f32 y, f32 z);
void GX_Color4u8(u8 r, u8 g, u8 b, u8 a); void GX_TexCoord2f32(f32 s, f32 t);
void GX_LoadPosMtxImm(Mtx m, u32 id); void GX_LoadNrmMtxImm(Mtx m, u32 id); void GX_SetCurrentMtx(u32 id);
void GX_LoadProjectionMtx(Mtx44 m, u8 type); void GX_LoadTexMtxImm(Mtx m, u32 id, u8 type);
void GX_SetNumChans(u8 n); void GX_SetChanCtrl(s32 chan, u8 enable, u8 amb, u8 mat, u8 lights, u8 diff, u8 attn);
void GX_SetNumTexGens(u32 n); void GX_SetTexCoordGen(u16 tc, u32 type, u32 src, u32 mtx);
void GX_SetNumIndStages(u8 n); void GX_SetNumTevStages(u8 n); void GX_SetTevOrder(u8 stage, u8 tc, u32 map, u8 color);
void GX_SetTevOp(u8 stage, u8 mode); void GX_SetTevDirect(u8 stage);
void GX_SetTevColorIn(u8 s, u8 a, u8 b, u8 c, u8 d); void GX_SetTevAlphaIn(u8 s, u8 a, u8 b, u8 c, u8 d);
void GX_SetTevColorOp(u8 s, u8 op, u8 bias, u8 scale, u8 clamp, u8 reg); void GX_SetTevAlphaOp(u8 s, u8 op, u8 bias, u8 scale, u8 clamp, u8 reg);
void GX_SetTevColor(u8 reg, GXColor c); void GX_SetTevKColor(u8 sel, GXColor c); void GX_SetTevKColorSel(u8 s, u8 sel); void GX_SetTevKAlphaSel(u8 s, u8 sel);
void GX_SetColorUpdate(u8 enable); void GX_SetBlendMode(u8 type, u8 src, u8 dst, u8 op); void GX_SetZMode(u8 enable, u8 func, u8 update);
void GX_SetAlphaCompare(u8 c0, u8 r0, u8 op, u8 c1, u8 r1); void GX_SetCullMode(u8 m); void GX_SetLineWidth(u8 w, u8 fmt);
void GX_InitTexObj(GXTexObj *o, void *img, u16 w, u16 h, u8 fmt, u8 ws, u8 wt, u8 mip);
void GX_InitTexObjLOD(GXTexObj *o, u8 minf, u8 magf, f32 minlod, f32 maxlod, f32 bias, u8 bc, u8 edge, u8 aniso);
void GX_LoadTexObj(GXTexObj *o, u8 map); void GX_InvalidateTexAll(void);
void GX_BeginDispList(void *list, u32 size); u32 GX_EndDispList(void); void GX_CallDispList(void *list, u32 n);
void guMtxIdentity(Mtx m); void guMtxCopy(Mtx a, Mtx b); void guMtxConcat(Mtx a, Mtx b, Mtx ab);
void guMtxRotRad(Mtx m, char axis, f32 r); void guMtxScale(Mtx m, f32 x, f32 y, f32 z); void guMtxTrans(Mtx m, f32 x, f32 y, f32 z);
void guPerspective(Mtx44 m, f32 fovy, f32 aspect, f32 n, f32 f); void guOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f);
#endif
