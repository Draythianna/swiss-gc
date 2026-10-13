/* A small software GX for previewing jewelcase.c on a PC.
   Follows GX's rules where jewelcase.c relies on them: perspective-correct
   interpolation, TEV as d + lerp(a, b, c) with scale and clamp, the GX blend
   factors and an RGB8 EFB with a depth buffer. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gccore.h"

#define W 640
#define H 480

float efb[H][W][3];
// per-frame counts for profiling: vertices and primitives sent, pixels drawn
unsigned long gx_statVerts, gx_statPixels, gx_statPrims;
int gx_noRaster;
static float zbuf[H][W];

static Mtx posMtx;
static Mtx44 proj;
static int projOrtho;
static Mtx texMtx[64];
static u8 desc[16];
static const float *arrPos, *arrNrm;
static int numChans = 1, numTexGens, numStages = 1;
static struct { int type, src, mtx; } texgen[2];
static struct {
	int tc, map, chan;
	int ca, cb, cc, cd, aa, ab, ac, ad;
	int cscale, ascale;
	int ksel, kasel;
} tev[4];
static float tevReg[4][4], kcolor[4][4];
static int blendType = 1, blendSrc = 4, blendDst = 5;
static int zEnable, zFunc = 7, zUpdate;
static float lineWidth = 1;
static GXTexObj *texmap[2];

/* --- matrices ------------------------------------------------------------ */

void guMtxIdentity(Mtx m) { memset(m, 0, sizeof(Mtx)); m[0][0] = m[1][1] = m[2][2] = 1; }
void guMtxCopy(Mtx a, Mtx b) { memmove(b, a, sizeof(Mtx)); }
void guMtxConcat(Mtx a, Mtx b, Mtx ab)
{
	Mtx t;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++) {
			t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0);
		}
	}
	memcpy(ab, t, sizeof(Mtx));
}
void guMtxRotRad(Mtx m, char axis, f32 r)
{
	float c = cosf(r), s = sinf(r);
	guMtxIdentity(m);
	if (axis == 'x' || axis == 'X') { m[1][1] = c; m[1][2] = -s; m[2][1] = s; m[2][2] = c; }
	if (axis == 'y' || axis == 'Y') { m[0][0] = c; m[0][2] = s; m[2][0] = -s; m[2][2] = c; }
	if (axis == 'z' || axis == 'Z') { m[0][0] = c; m[0][1] = -s; m[1][0] = s; m[1][1] = c; }
}
void guMtxScale(Mtx m, f32 x, f32 y, f32 z) { guMtxIdentity(m); m[0][0] = x; m[1][1] = y; m[2][2] = z; }
void guMtxTrans(Mtx m, f32 x, f32 y, f32 z) { guMtxIdentity(m); m[0][3] = x; m[1][3] = y; m[2][3] = z; }
void guPerspective(Mtx44 m, f32 fovy, f32 aspect, f32 n, f32 f)
{
	float cot = 1.0f / tanf(fovy * 0.5f * (float)M_PI / 180.0f), tmp = 1.0f / (f - n);
	memset(m, 0, sizeof(Mtx44));
	m[0][0] = cot / aspect; m[1][1] = cot;
	m[2][2] = -n * tmp; m[2][3] = -(f * n) * tmp; m[3][2] = -1;
}
void guOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
	memset(m, 0, sizeof(Mtx44));
	m[0][0] = 2 / (r - l); m[0][3] = -(r + l) / (r - l);
	m[1][1] = 2 / (t - b); m[1][3] = -(t + b) / (t - b);
	m[2][2] = -1 / (f - n); m[2][3] = -f / (f - n); m[3][3] = 1;
}

/* --- state --------------------------------------------------------------- */

void GX_ClearVtxDesc(void) { memset(desc, 0, sizeof(desc)); }
void GX_SetVtxDesc(u8 attr, u8 type) { if (attr < 16) desc[attr] = type; }
void GX_SetVtxAttrFmt(u8 f, u32 a, u32 c, u32 t, u8 fr) { (void)f; (void)a; (void)c; (void)t; (void)fr; }
void GX_SetArray(u32 attr, const void *ptr, u8 stride) { (void)stride; if (attr == GX_VA_POS) arrPos = ptr; if (attr == GX_VA_NRM) arrNrm = ptr; }
void GX_LoadPosMtxImm(Mtx m, u32 id) { (void)id; memcpy(posMtx, m, sizeof(Mtx)); }
void GX_LoadNrmMtxImm(Mtx m, u32 id) { (void)m; (void)id; }
void GX_SetCurrentMtx(u32 id) { (void)id; }
void GX_LoadProjectionMtx(Mtx44 m, u8 type) { memcpy(proj, m, sizeof(Mtx44)); projOrtho = type == GX_ORTHOGRAPHIC; }
void GX_LoadTexMtxImm(Mtx m, u32 id, u8 type) { (void)type; if (id < 64) memcpy(texMtx[id], m, sizeof(Mtx)); }
void GX_SetNumChans(u8 n) { numChans = n; }
void GX_SetChanCtrl(s32 c, u8 e, u8 a, u8 m, u8 l, u8 d, u8 at) { (void)c; (void)e; (void)a; (void)m; (void)l; (void)d; (void)at; }
void GX_SetNumTexGens(u32 n) { numTexGens = n; }
void GX_SetTexCoordGen(u16 tc, u32 type, u32 src, u32 mtx) { if (tc < 2) { texgen[tc].type = type; texgen[tc].src = src; texgen[tc].mtx = mtx; } }
void GX_SetNumIndStages(u8 n) { (void)n; }
void GX_SetNumTevStages(u8 n) { numStages = n; }
void GX_SetTevOrder(u8 s, u8 tc, u32 map, u8 color) { tev[s].tc = tc; tev[s].map = map; tev[s].chan = color; }
void GX_SetTevDirect(u8 s) { (void)s; }
void GX_SetTevColorIn(u8 s, u8 a, u8 b, u8 c, u8 d) { tev[s].ca = a; tev[s].cb = b; tev[s].cc = c; tev[s].cd = d; }
void GX_SetTevAlphaIn(u8 s, u8 a, u8 b, u8 c, u8 d) { tev[s].aa = a; tev[s].ab = b; tev[s].ac = c; tev[s].ad = d; }
void GX_SetTevColorOp(u8 s, u8 op, u8 bias, u8 scale, u8 clamp, u8 reg) { (void)op; (void)bias; (void)clamp; (void)reg; tev[s].cscale = scale; }
void GX_SetTevAlphaOp(u8 s, u8 op, u8 bias, u8 scale, u8 clamp, u8 reg) { (void)op; (void)bias; (void)clamp; (void)reg; tev[s].ascale = scale; }
void GX_SetTevOp(u8 s, u8 mode)
{
	tev[s].cscale = tev[s].ascale = 0;
	if (mode == GX_MODULATE) { GX_SetTevColorIn(s, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO); GX_SetTevAlphaIn(s, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO); }
	if (mode == GX_PASSCLR) { GX_SetTevColorIn(s, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC); GX_SetTevAlphaIn(s, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA); }
	if (mode == GX_REPLACE) { GX_SetTevColorIn(s, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC); GX_SetTevAlphaIn(s, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA); }
}
static void setC(float *d, GXColor c) { d[0] = c.r / 255.0f; d[1] = c.g / 255.0f; d[2] = c.b / 255.0f; d[3] = c.a / 255.0f; }
void GX_SetTevColor(u8 reg, GXColor c) { setC(tevReg[reg], c); }
void GX_SetTevKColor(u8 sel, GXColor c) { setC(kcolor[sel], c); }
void GX_SetTevKColorSel(u8 s, u8 sel) { tev[s].ksel = sel; }
void GX_SetTevKAlphaSel(u8 s, u8 sel) { tev[s].kasel = sel; }
void GX_SetBlendMode(u8 type, u8 src, u8 dst, u8 op) { (void)op; blendType = type; blendSrc = src; blendDst = dst; }
void GX_SetZMode(u8 e, u8 f, u8 u) { zEnable = e; zFunc = f; zUpdate = u; }
static u8 colorUpdate = 1;
void GX_SetColorUpdate(u8 e) { colorUpdate = e; }
void GX_SetAlphaCompare(u8 a, u8 b, u8 c, u8 d, u8 e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void GX_SetCullMode(u8 m) { (void)m; }
void GX_SetLineWidth(u8 w, u8 f) { (void)f; lineWidth = w / 6.0f; }
void GX_InitTexObj(GXTexObj *o, void *img, u16 w, u16 h, u8 fmt, u8 ws, u8 wt, u8 mip)
{
	memset(o, 0, sizeof(*o));
	o->data = img; o->w = w; o->h = h; o->fmt = fmt; o->wrapS = ws; o->wrapT = wt; o->mip = mip;
	o->minFilt = mip ? GX_LIN_MIP_LIN : GX_LINEAR; o->magFilt = GX_LINEAR;
}
void GX_InitTexObjLOD(GXTexObj *o, u8 minf, u8 magf, f32 minl, f32 maxl, f32 b, u8 bc, u8 e, u8 a)
{
	(void)b; (void)bc; (void)e; (void)a;
	o->minFilt = minf; o->magFilt = magf; o->minLod = minl; o->maxLod = maxl;
}
void GX_LoadTexObj(GXTexObj *o, u8 map) { if (map < 2) texmap[map] = o; }
void GX_InvalidateTexAll(void) {}

/* --- textures ------------------------------------------------------------ */

static void texel(const GXTexObj *t, const u8 *base, int w, int h, int x, int y, float *out)
{
	if (t->wrapS == GX_REPEAT) x = ((x % w) + w) % w; else x = x < 0 ? 0 : x >= w ? w - 1 : x;
	if (t->wrapT == GX_REPEAT) y = ((y % h) + h) % h; else y = y < 0 ? 0 : y >= h ? h - 1 : y;
	if (t->fmt == GX_TF_RGBA8) {
		int tile = (y >> 2) * ((w + 3) >> 2) + (x >> 2), off = tile * 64 + ((y & 3) * 4 + (x & 3)) * 2;
		out[3] = base[off] / 255.0f; out[0] = base[off + 1] / 255.0f; out[1] = base[off + 32] / 255.0f; out[2] = base[off + 33] / 255.0f;
	}
	else {
		int tile = (y >> 2) * ((w + 7) >> 3) + (x >> 3);
		float v = base[tile * 32 + (y & 3) * 8 + (x & 7)] / 255.0f;
		out[0] = out[1] = out[2] = out[3] = v;
	}
}

static void sampleLevel(const GXTexObj *t, int level, float s, float u, int linear, float *out)
{
	const u8 *base = t->data;
	int w = t->w, h = t->h;
	for (int l = 0; l < level; l++) {
		base += ((w + 3) & ~3) * ((h + 3) & ~3) * (t->fmt == GX_TF_RGBA8 ? 4 : 1);
		w = w > 1 ? w / 2 : 1;
		h = h > 1 ? h / 2 : 1;
	}
	float x = s * w - 0.5f, y = u * h - 0.5f;
	if (!linear) { texel(t, base, w, h, (int)floorf(x + 0.5f), (int)floorf(y + 0.5f), out); return; }
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - x0, fy = y - y0, a[4], b[4], c[4], d[4];
	texel(t, base, w, h, x0, y0, a); texel(t, base, w, h, x0 + 1, y0, b);
	texel(t, base, w, h, x0, y0 + 1, c); texel(t, base, w, h, x0 + 1, y0 + 1, d);
	for (int i = 0; i < 4; i++) out[i] = (a[i] * (1 - fx) + b[i] * fx) * (1 - fy) + (c[i] * (1 - fx) + d[i] * fx) * fy;
}

static void sampleTex(const GXTexObj *t, float s, float u, float lod, float *out)
{
	if (!t || !t->data) { out[0] = out[1] = out[2] = out[3] = 0; return; }
	if (!t->mip || lod <= 0) { sampleLevel(t, 0, s, u, lod <= 0 ? t->magFilt != GX_NEAR : t->minFilt != GX_NEAR, out); return; }
	if (lod > t->maxLod) lod = t->maxLod;
	int l0 = (int)floorf(lod);
	float f = lod - l0, a[4], b[4];
	sampleLevel(t, l0, s, u, 1, a);
	if (t->minFilt == GX_LIN_MIP_LIN && l0 + 1 <= t->maxLod) {
		sampleLevel(t, l0 + 1, s, u, 1, b);
		for (int i = 0; i < 4; i++) out[i] = a[i] * (1 - f) + b[i] * f;
	}
	else memcpy(out, a, sizeof(a));
}

/* --- vertices and primitives -------------------------------------------- */

typedef struct { float x, y, z, w; float sx, sy, sz, iw; float c[4]; float tc[2]; } vtx_t;

#define MAX_VTX 65536
static vtx_t vbuf[MAX_VTX];
static int vcount, prim;
static float curPos[3], curNrm[3], curClr[4], curTex[2];
static int havePos;

static void finishVertex(void)
{
	if (!havePos || vcount >= MAX_VTX) return;
	vtx_t *v = &vbuf[vcount++];
	float p[3];
	for (int i = 0; i < 3; i++) p[i] = posMtx[i][0] * curPos[0] + posMtx[i][1] * curPos[1] + posMtx[i][2] * curPos[2] + posMtx[i][3];
	float cl[4];
	for (int i = 0; i < 4; i++) cl[i] = proj[i][0] * p[0] + proj[i][1] * p[1] + proj[i][2] * p[2] + proj[i][3];
	if (projOrtho) cl[3] = 1;
	v->x = cl[0]; v->y = cl[1]; v->z = cl[2]; v->w = cl[3];
	v->iw = 1.0f / cl[3];
	v->sx = (cl[0] * v->iw * 0.5f + 0.5f) * W;
	v->sy = (0.5f - cl[1] * v->iw * 0.5f) * H;
	v->sz = cl[2] * v->iw;
	memcpy(v->c, curClr, sizeof(curClr));
	// texture coordinate generation
	v->tc[0] = v->tc[1] = 0;
	if (numTexGens > 0) {
		float in[4] = {0, 0, 1, 1};
		if (texgen[0].src == GX_TG_NRM) { in[0] = curNrm[0]; in[1] = curNrm[1]; in[2] = curNrm[2]; }
		else { in[0] = curTex[0]; in[1] = curTex[1]; }
		if (texgen[0].mtx == GX_IDENTITY) { v->tc[0] = in[0]; v->tc[1] = in[1]; }
		else {
			float (*m)[4] = texMtx[texgen[0].mtx];
			for (int i = 0; i < 2; i++) v->tc[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2] + m[i][3] * in[3];
		}
	}
	havePos = 0;
}

void GX_Begin(u8 p, u8 fmt, u16 n);
static u8 *dlBuf; static u32 dlLen, dlCap;
static void dlPut(const void *p, int n) { if (dlBuf && dlLen + n <= dlCap) { memcpy(dlBuf + dlLen, p, n); } dlLen += n; }

void GX_Position3f32(f32 x, f32 y, f32 z) { finishVertex(); curPos[0] = x; curPos[1] = y; curPos[2] = z; havePos = 1; }
void GX_Position1x16(u16 i)
{
	if (dlBuf) { dlPut(&i, 2); return; }
	finishVertex();
	memcpy(curPos, arrPos + i * 3, 12);
	havePos = 1;
}
void GX_Normal1x16(u16 i) { if (dlBuf) { dlPut(&i, 2); return; } memcpy(curNrm, arrNrm + i * 3, 12); }
void GX_Normal3f32(f32 x, f32 y, f32 z) { curNrm[0] = x; curNrm[1] = y; curNrm[2] = z; }
void GX_Color4u8(u8 r, u8 g, u8 b, u8 a) { curClr[0] = r / 255.0f; curClr[1] = g / 255.0f; curClr[2] = b / 255.0f; curClr[3] = a / 255.0f; }
void GX_TexCoord2f32(f32 s, f32 t) { curTex[0] = s; curTex[1] = t; }

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static void colorIn(int sel, const float *prev, const float *tex, const float *ras, const float *k, float *o)
{
	switch (sel) {
		case GX_CC_CPREV: memcpy(o, prev, 12); break;
		case GX_CC_APREV: o[0] = o[1] = o[2] = prev[3]; break;
		case GX_CC_C0: memcpy(o, tevReg[1], 12); break;
		case GX_CC_A0: o[0] = o[1] = o[2] = tevReg[1][3]; break;
		case GX_CC_TEXC: memcpy(o, tex, 12); break;
		case GX_CC_TEXA: o[0] = o[1] = o[2] = tex[3]; break;
		case GX_CC_RASC: memcpy(o, ras, 12); break;
		case GX_CC_RASA: o[0] = o[1] = o[2] = ras[3]; break;
		case GX_CC_ONE: o[0] = o[1] = o[2] = 1; break;
		case GX_CC_HALF: o[0] = o[1] = o[2] = 0.5f; break;
		case GX_CC_KONST: memcpy(o, k, 12); break;
		default: o[0] = o[1] = o[2] = 0; break;
	}
}

static float alphaIn(int sel, const float *prev, const float *tex, const float *ras, float k)
{
	switch (sel) {
		case GX_CA_APREV: return prev[3];
		case GX_CA_A0: return tevReg[1][3];
		case GX_CA_TEXA: return tex[3];
		case GX_CA_RASA: return ras[3];
		case GX_CA_KONST: return k;
		default: return 0;
	}
}

static void shade(const float *ras, float s, float t, float lod, float *out)
{
	float prev[4] = {0, 0, 0, 0};
	for (int st = 0; st < numStages; st++) {
		float tex[4] = {0, 0, 0, 0}, r[4] = {0, 0, 0, 0};
		if (tev[st].map != GX_TEXMAP_NULL && tev[st].tc != GX_TEXCOORDNULL) sampleTex(texmap[tev[st].map], s, t, lod, tex);
		if (tev[st].chan != GX_COLORNULL && numChans > 0) memcpy(r, ras, sizeof(r));
		float k[4] = {1, 1, 1, 1}, ka = 1;
		if (tev[st].ksel == GX_TEV_KCSEL_K0) memcpy(k, kcolor[0], 12);
		if (tev[st].kasel == GX_TEV_KASEL_K0_A) ka = kcolor[0][3];
		float a[3], b[3], c[3], d[3], o[4];
		colorIn(tev[st].ca, prev, tex, r, k, a); colorIn(tev[st].cb, prev, tex, r, k, b);
		colorIn(tev[st].cc, prev, tex, r, k, c); colorIn(tev[st].cd, prev, tex, r, k, d);
		float cs = tev[st].cscale == GX_CS_SCALE_2 ? 2 : tev[st].cscale == GX_CS_SCALE_4 ? 4 : tev[st].cscale == GX_CS_DIVIDE_2 ? 0.5f : 1;
		for (int i = 0; i < 3; i++) o[i] = clamp01((d[i] + a[i] * (1 - c[i]) + b[i] * c[i]) * cs);
		float aa = alphaIn(tev[st].aa, prev, tex, r, ka), ab = alphaIn(tev[st].ab, prev, tex, r, ka);
		float ac = alphaIn(tev[st].ac, prev, tex, r, ka), ad = alphaIn(tev[st].ad, prev, tex, r, ka);
		float as = tev[st].ascale == GX_CS_SCALE_2 ? 2 : 1;
		o[3] = clamp01((ad + aa * (1 - ac) + ab * ac) * as);
		// TEV works in 8 bits
		for (int i = 0; i < 4; i++) prev[i] = roundf(o[i] * 255) / 255;
	}
	memcpy(out, prev, sizeof(prev));
}

static float factor(int f, bool isSrc, const float *src, const float *dst, int i)
{
	switch (f) {
		case GX_BL_ZERO: return 0;
		case GX_BL_ONE: return 1;
		case GX_BL_SRCCLR: return isSrc ? dst[i] : src[i];
		case GX_BL_INVSRCCLR: return isSrc ? 1 - dst[i] : 1 - src[i];
		case GX_BL_SRCALPHA: return src[3];
		case GX_BL_INVSRCALPHA: return 1 - src[3];
		default: return 1;	// no destination alpha in an RGB8 EFB
	}
}

static void plot(int x, int y, float z, const float *src)
{
	if (x < 0 || y < 0 || x >= W || y >= H) return;
	if (zEnable) {
		float old = zbuf[y][x];
		bool pass = zFunc == GX_ALWAYS || (zFunc == GX_LEQUAL && z <= old) || (zFunc == GX_LESS && z < old);
		if (!pass) return;
		if (zUpdate) zbuf[y][x] = z;
	}
	gx_statPixels++;
	if (!colorUpdate) return;
	float *d = efb[y][x], out[3];
	if (blendType == GX_BM_NONE) for (int i = 0; i < 3; i++) out[i] = src[i];
	else {
		float dst[4] = {d[0], d[1], d[2], 1};
		for (int i = 0; i < 3; i++) out[i] = src[i] * factor(blendSrc, true, src, dst, i) + d[i] * factor(blendDst, false, src, dst, i);
	}
	for (int i = 0; i < 3; i++) d[i] = roundf(clamp01(out[i]) * 255) / 255;
}

static float edge(float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }

static void triangle(const vtx_t *a, const vtx_t *b, const vtx_t *c)
{
	if (a->w <= 1e-3f || b->w <= 1e-3f || c->w <= 1e-3f) return;
	float area = edge(a->sx, a->sy, b->sx, b->sy, c->sx, c->sy);
	if (fabsf(area) < 1e-9f) return;
	int x0 = (int)floorf(fminf(a->sx, fminf(b->sx, c->sx))), x1 = (int)ceilf(fmaxf(a->sx, fmaxf(b->sx, c->sx)));
	int y0 = (int)floorf(fminf(a->sy, fminf(b->sy, c->sy))), y1 = (int)ceilf(fmaxf(a->sy, fmaxf(b->sy, c->sy)));
	if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W - 1) x1 = W - 1; if (y1 > H - 1) y1 = H - 1;
	// level of detail for the whole triangle, from the texture coordinate's screen gradient
	float lod = 0;
	if (numTexGens > 0 && tev[0].map != GX_TEXMAP_NULL && texmap[tev[0].map]) {
		const GXTexObj *t = texmap[tev[0].map];
		float du1 = (b->tc[0] - a->tc[0]) * t->w, dv1 = (b->tc[1] - a->tc[1]) * t->h;
		float du2 = (c->tc[0] - a->tc[0]) * t->w, dv2 = (c->tc[1] - a->tc[1]) * t->h;
		float texArea = fabsf(du1 * dv2 - du2 * dv1);
		lod = texArea > 0 ? 0.5f * log2f(texArea / fabsf(area)) : 0;
	}
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			float px = x + 0.5f, py = y + 0.5f;
			float w0 = edge(b->sx, b->sy, c->sx, c->sy, px, py) / area;
			float w1 = edge(c->sx, c->sy, a->sx, a->sy, px, py) / area;
			float w2 = 1 - w0 - w1;
			if (w0 < 0 || w1 < 0 || w2 < 0) continue;
			float iw = w0 * a->iw + w1 * b->iw + w2 * c->iw;
			float k0 = w0 * a->iw / iw, k1 = w1 * b->iw / iw, k2 = w2 * c->iw / iw;
			float ras[4], col[4];
			for (int i = 0; i < 4; i++) ras[i] = a->c[i] * k0 + b->c[i] * k1 + c->c[i] * k2;
			float s = a->tc[0] * k0 + b->tc[0] * k1 + c->tc[0] * k2, t = a->tc[1] * k0 + b->tc[1] * k1 + c->tc[1] * k2;
			shade(ras, s, t, lod, col);
			plot(x, y, w0 * a->sz + w1 * b->sz + w2 * c->sz, col);
		}
	}
}

static void line(const vtx_t *a, const vtx_t *b)
{
	float dx = b->sx - a->sx, dy = b->sy - a->sy, l = sqrtf(dx * dx + dy * dy);
	if (l < 1e-6f) return;
	float nx = -dy / l * lineWidth * 0.5f, ny = dx / l * lineWidth * 0.5f;
	vtx_t q[4] = {*a, *a, *b, *b};
	q[0].sx += nx; q[0].sy += ny; q[1].sx -= nx; q[1].sy -= ny;
	q[2].sx -= nx; q[2].sy -= ny; q[3].sx += nx; q[3].sy += ny;
	triangle(&q[0], &q[1], &q[2]);
	triangle(&q[0], &q[2], &q[3]);
}

static void flush(void)
{
	finishVertex();
	vtx_t *v = vbuf;
	int n = vcount;
	gx_statVerts += n;
	gx_statPrims++;
	if (gx_noRaster) { vcount = 0; return; }
	switch (prim) {
		case GX_TRIANGLES: for (int i = 0; i + 2 < n; i += 3) triangle(&v[i], &v[i + 1], &v[i + 2]); break;
		case GX_TRIANGLESTRIP: for (int i = 0; i + 2 < n; i++) triangle(&v[i], &v[i + 1], &v[i + 2]); break;
		case GX_TRIANGLEFAN: for (int i = 1; i + 1 < n; i++) triangle(&v[0], &v[i], &v[i + 1]); break;
		case GX_QUADS: for (int i = 0; i + 3 < n; i += 4) { triangle(&v[i], &v[i + 1], &v[i + 2]); triangle(&v[i], &v[i + 2], &v[i + 3]); } break;
		case GX_LINESTRIP: for (int i = 0; i + 1 < n; i++) line(&v[i], &v[i + 1]); break;
		case GX_LINES: for (int i = 0; i + 1 < n; i += 2) line(&v[i], &v[i + 1]); break;
	}
	vcount = 0;
}

void GX_Begin(u8 p, u8 fmt, u16 n)
{
	(void)fmt;
	if (dlBuf) { u8 h[3] = {p, (u8)(n >> 8), (u8)n}; dlPut(h, 3); return; }
	prim = p; vcount = 0; havePos = 0;
}
void GX_End(void) { if (!dlBuf) flush(); }

void GX_BeginDispList(void *list, u32 size) { dlBuf = list; dlCap = size; dlLen = 0; }
u32 GX_EndDispList(void)
{
	u32 n = dlLen;
	if (n > dlCap) { fprintf(stderr, "display list overflow %u > %u\n", n, dlCap); n = 0; }
	dlBuf = NULL;
	return n;
}
void GX_CallDispList(void *list, u32 size)
{
	// the GPU runs display lists: no CPU time to measure when profiling
	if (gx_noRaster) return;
	const u8 *p = list, *end = p + size;
	while (p + 3 <= end) {
		int type = p[0], n = (p[1] << 8) | p[2];
		p += 3;
		if (!type) break;
		GX_Begin(type, 0, n);
		for (int i = 0; i < n; i++) {
			u16 pi, ni;
			memcpy(&pi, p, 2); memcpy(&ni, p + 2, 2);
			p += 4;
			GX_Position1x16(pi);
			GX_Normal1x16(ni);
		}
		GX_End();
	}
}

void gx_clear(float r, float g, float b)
{
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { efb[y][x][0] = r; efb[y][x][1] = g; efb[y][x][2] = b; zbuf[y][x] = 1e30f; }
}

// Swiss's backdrop without its cubes and orbs: THEME_BG_TOP to MID over 260 lines, then to BOTTOM
void gx_backdrop(void)
{
	static const float top[3] = {28, 22, 74}, mid[3] = {14, 11, 44}, bot[3] = {5, 4, 18};
	for (int y = 0; y < H; y++) {
		float c[3];
		for (int i = 0; i < 3; i++) c[i] = (y < 260 ? top[i] + (mid[i] - top[i]) * y / 260.0f : mid[i] + (bot[i] - mid[i]) * (y - 260) / 220.0f) / 255.0f;
		for (int x = 0; x < W; x++) { efb[y][x][0] = c[0]; efb[y][x][1] = c[1]; efb[y][x][2] = c[2]; }
	}
}

// The copy to the screen at the end of a frame: as on the GameCube, it clears
// depth only if depth writes are on at the time
void gx_copy_clear(void)
{
	if (!zUpdate) return;
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) zbuf[y][x] = 1e30f;
}

void gx_save_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int i = 0; i < 3; i++) fputc((int)roundf(efb[y][x][i] * 255), f);
	fclose(f);
}
