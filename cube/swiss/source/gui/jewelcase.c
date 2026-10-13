/* -----------------------------------------------------------
      jewelcase.c - games shown as clear CD jewel cases

      A port of bubbbly.com/cd-case to GX. The case is built
      the same way, in millimetres: every edge, rib and hinge
      is a "rod" (a rounded profile swept along a path) and the
      flat parts are thin clear sheets. The original traces
      light through the plastic per pixel; here the rods look
      it up in a matcap baked from the same studio at start-up,
      and the sheets, the disc and its rainbow are lit per
      vertex against that studio.

      Timings, poses and curves are the original's.
   ----------------------------------------------------------- */

#include <math.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <gccore.h>
#include "jewelcase.h"
#include "pngdec.h"
#include "scene3d.h"
#include "deviceHandler.h"
#include "files.h"
#include "swiss.h"

#define VTXFMT_ROD  GX_VTXFMT5
#define VTXFMT_DIR  GX_VTXFMT6

#define PI_F 3.14159265f

/* ---------------------------------------------------------------------------
 * Small maths
 * ------------------------------------------------------------------------- */

typedef struct { float x, y, z; } v3;

static inline v3 V3(float x, float y, float z) { v3 r = {x, y, z}; return r; }
static inline v3 vadd(v3 a, v3 b) { return V3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 vsub(v3 a, v3 b) { return V3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3 vmul(v3 a, float s) { return V3(a.x * s, a.y * s, a.z * s); }
static inline float vdot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float vlen(v3 a) { return sqrtf(vdot(a, a)); }
static inline v3 vnorm(v3 a) { float l = vlen(a); return l > 1e-12f ? vmul(a, 1.0f / l) : a; }
static inline v3 vmix(v3 a, v3 b, float t) { return vadd(a, vmul(vsub(b, a), t)); }
static inline v3 vmin(v3 a, float m) { return V3(fminf(a.x, m), fminf(a.y, m), fminf(a.z, m)); }
static inline v3 vreflect(v3 i, v3 n) { return vsub(i, vmul(n, 2.0f * vdot(i, n))); }

static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float sat(float x) { return clampf(x, 0.0f, 1.0f); }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smoothstep(float a, float b, float x)
{
	float t = sat((x - a) / (b - a));
	return t * t * (3.0f - 2.0f * t);
}

static void xform(Mtx m, v3 p, v3 *out)
{
	out->x = m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3];
	out->y = m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3];
	out->z = m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3];
}

static void xformDir(Mtx m, v3 p, v3 *out)
{
	out->x = m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z;
	out->y = m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z;
	out->z = m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z;
}

// Linear light to sRGB through a table, the original renders in linear light
static u8 srgbLut[1025];
static void initSrgb(void)
{
	for (int i = 0; i <= 1024; i++) {
		float c = i / 1024.0f;
		float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
		srgbLut[i] = (u8)lrintf(sat(s) * 255.0f);
	}
}
static inline float toSrgb(float c) { return srgbLut[(int)(sat(c) * 1024.0f)] / 255.0f; }
static inline u8 toSrgb8(float c) { return srgbLut[(int)(sat(c) * 1024.0f)]; }

/* ---------------------------------------------------------------------------
 * The studio, as the original's shaders describe it (view space, z towards
 * the camera): a grey room hung with black flags and hot strip lights behind
 * the case, a white backdrop straight behind it, softboxes round the camera.
 * ------------------------------------------------------------------------- */

static float boxf(float px, float py, float cx, float cy, float hx, float hy, float soft)
{
	float qx = fabsf(px - cx) - hx, qy = fabsf(py - cy) - hy;
	float mx = fmaxf(qx, 0.0f), my = fmaxf(qy, 0.0f);
	float d = sqrtf(mx * mx + my * my) + fminf(fmaxf(qx, qy), 0.0f);
	return 1.0f - smoothstep(-soft, soft, d);
}

static float band(float q, float c, float hw, float soft)
{
	return 1.0f - smoothstep(hw - soft, hw + soft, fabsf(q - c));
}

static float flags(float q)
{
	float k = band(q, 0.27f, 0.07f, 0.02f);
	k = fmaxf(k, band(q, 0.74f, 0.12f, 0.03f));
	k = fmaxf(k, band(q, -0.32f, 0.08f, 0.02f));
	k = fmaxf(k, band(q, -0.86f, 0.14f, 0.03f));
	return k;
}

static float hot(float q)
{
	return band(q, 0.48f, 0.07f, 0.02f) + band(q, 1.08f, 0.14f, 0.03f) + band(q, -0.56f, 0.08f, 0.02f) +
		0.8f * band(q, -1.25f, 0.15f, 0.03f);
}

static v3 room(v3 d)
{
	d = vnorm(d);
	float k = 1.0f / fmaxf(fabsf(d.z), 0.08f);
	float px = d.x * k, py = d.y * k;
	float lp = sqrtf(px * px + py * py);
	v3 dim = V3(0.02f, 0.021f, 0.026f);
	if (d.z < 0.0f) {
		v3 c = vmul(V3(0.1f, 0.103f, 0.112f), 1.0f - 0.5f * smoothstep(0.4f, 1.6f, lp));
		c = vadd(c, vmul(V3(0.8f, 0.8f, 0.8f), 1.0f - smoothstep(0.05f, 0.14f, lp)));
		c = vmul(c, 1.0f - 0.97f * fmaxf(flags(px), flags(py)));
		c = vadd(c, vmul(V3(2.4f, 2.6f, 3.0f), hot(px) + hot(py)));
		return vmix(dim, c, smoothstep(0.0f, 0.2f, -d.z));
	}
	v3 c = vmul(dim, 0.5f + 0.5f * smoothstep(-1.2f, 1.6f, py));
	float s = 0.38f * smoothstep(-0.7f, 0.8f, py) * (1.0f - smoothstep(0.5f, 1.0f, lp));
	c = vadd(c, V3(s, s, s));
	c = vadd(c, vmul(V3(1.0f, 0.99f, 0.97f), 24.0f * boxf(px, py, -0.86f, 0.3f, 0.065f, 0.85f, 0.035f)));
	c = vadd(c, vmul(V3(0.96f, 0.98f, 1.0f), 20.0f * boxf(px, py, 1.06f, 0.04f, 0.06f, 1.15f, 0.03f)));
	float t = 13.0f * boxf(px, py, 0.05f, 1.28f, 1.25f, 0.055f, 0.03f);
	c = vadd(c, V3(t, t, t));
	c = vadd(c, vmul(V3(0.92f, 0.94f, 0.97f), 2.4f * boxf(px, py, 0.1f, -1.05f, 1.15f, 0.24f, 0.28f)));
	c = vmul(c, 1.0f - 0.92f * boxf(px, py, -0.2f, 0.5f, 0.055f, 1.4f, 0.02f));
	c = vmul(c, 1.0f - 0.85f * boxf(px, py, 0.62f, -0.35f, 0.04f, 0.9f, 0.02f));
	return vmix(dim, c, smoothstep(0.0f, 0.2f, d.z));
}

static float backdropOf(v3 d)
{
	d = vnorm(d);
	if (d.z >= 0.0f) return 0.0f;
	float k = 1.0f / fmaxf(fabsf(d.z), 0.08f);
	float px = d.x * k * 0.92f, py = d.y * k;
	float lit = 1.0f - smoothstep(0.3f, 0.68f, sqrtf(px * px + py * py));
	return lit * smoothstep(0.0f, 0.2f, -d.z);
}

// The room, baked into a small cube map as the original does, for what is lit
// every frame: looked up and interpolated it costs a fraction of working it out
#define ROOM_RES 48
static v3 *roomTable;
static v3 *studioTable;		// the room with the page behind, as studio() gives it

static void faceDir(int f, float u, float v, v3 *d)
{
	switch (f) {
		case 0: *d = V3(1, v, -u); break;
		case 1: *d = V3(-1, v, u); break;
		case 2: *d = V3(u, 1, -v); break;
		case 3: *d = V3(u, -1, v); break;
		case 4: *d = V3(u, v, 1); break;
		default: *d = V3(-u, v, -1); break;
	}
}

static v3 studioSlow(v3 d);

static void buildRoomTable(void)
{
	roomTable = malloc(6 * ROOM_RES * ROOM_RES * sizeof(v3));
	studioTable = malloc(6 * ROOM_RES * ROOM_RES * sizeof(v3));
	for (int f = 0; f < 6; f++) {
		for (int y = 0; y < ROOM_RES; y++) {
			for (int x = 0; x < ROOM_RES; x++) {
				v3 d;
				faceDir(f, (x + 0.5f) / ROOM_RES * 2 - 1, (y + 0.5f) / ROOM_RES * 2 - 1, &d);
				roomTable[(f * ROOM_RES + y) * ROOM_RES + x] = room(d);
			}
		}
	}
	for (int f = 0; f < 6; f++) {
		for (int y = 0; y < ROOM_RES; y++) {
			for (int x = 0; x < ROOM_RES; x++) {
				v3 d;
				faceDir(f, (x + 0.5f) / ROOM_RES * 2 - 1, (y + 0.5f) / ROOM_RES * 2 - 1, &d);
				studioTable[(f * ROOM_RES + y) * ROOM_RES + x] = studioSlow(d);
			}
		}
	}
}

// A direction looked up in one of the cube map tables, bilinearly
static v3 cubeLookup(const v3 *table, v3 d)
{
	float ax = fabsf(d.x), ay = fabsf(d.y), az = fabsf(d.z), u, v, m;
	int f;
	if (ax >= ay && ax >= az) { f = d.x > 0 ? 0 : 1; m = ax; u = d.x > 0 ? -d.z : d.z; v = d.y; }
	else if (ay >= az) { f = d.y > 0 ? 2 : 3; m = ay; u = d.x; v = d.y > 0 ? -d.z : d.z; }
	else { f = d.z > 0 ? 4 : 5; m = az; u = d.z > 0 ? d.x : -d.x; v = d.y; }
	float fx = clampf((u / m + 1) * 0.5f * ROOM_RES - 0.5f, 0, ROOM_RES - 1.001f);
	float fy = clampf((v / m + 1) * 0.5f * ROOM_RES - 0.5f, 0, ROOM_RES - 1.001f);
	int x0 = (int)fx, y0 = (int)fy;
	float tx = fx - x0, ty = fy - y0;
	const v3 *t = table + (f * ROOM_RES + y0) * ROOM_RES + x0;
	v3 a = vmix(t[0], t[1], tx), b = vmix(t[ROOM_RES], t[ROOM_RES + 1], tx);
	return vmix(a, b, ty);
}

static v3 roomT(v3 d) { return cubeLookup(roomTable, d); }

static v3 studioSlow(v3 d)
{
	return vmix(roomT(d), V3(1.0f, 1.0f, 1.0f), backdropOf(d));
}

static v3 studio(v3 d) { return cubeLookup(studioTable, d); }

static v3 glare(v3 d)
{
	v3 s = studio(d);
	return V3(fmaxf(s.x - 0.45f, 0.0f), fmaxf(s.y - 0.45f, 0.0f), fmaxf(s.z - 0.45f, 0.0f));
}

static const v3 KEY    = {-0.625f, 0.218f, 0.75f};
static const v3 STRIP  = { 0.725f, 0.028f, 0.688f};
static const v3 TOP    = { 0.031f, 0.786f, 0.618f};
static const v3 BOUNCE = { 0.069f, -0.722f, 0.688f};

static float fresnel(float cosv, float n)
{
	float f0 = (n - 1.0f) / (n + 1.0f);
	f0 *= f0;
	float m = 1.0f - sat(cosv);
	return f0 + (1.0f - f0) * m * m * m * m * m;
}

static float lights(v3 n)
{
	return 0.42f + 0.62f * fmaxf(vdot(n, KEY), 0.0f) + 0.28f * fmaxf(vdot(n, STRIP), 0.0f) +
		0.18f * fmaxf(vdot(n, TOP), 0.0f);
}

// A wavelength (nm) as a colour: Zucconi's fit of the visible spectrum
static float bump(float x, float y0) { return sat(1.0f - x * x - y0); }
static v3 spectral(float w)
{
	if (w < 380.0f || w > 760.0f) return V3(0, 0, 0);
	float x = sat((w - 400.0f) / 300.0f);
	return V3(
		bump(3.54585104f * (x - 0.69549072f), 0.02312639f) + bump(3.90307140f * (x - 0.11748627f), 0.84897130f),
		bump(2.93225262f * (x - 0.49228336f), 0.15225084f) + bump(3.21182957f * (x - 0.86755042f), 0.88445281f),
		bump(2.41593945f * (x - 0.27699880f), 0.52607955f) + bump(3.96587128f * (x - 0.66077860f), 0.73949448f));
}

#define IOR 1.59f

/* ---------------------------------------------------------------------------
 * Rod matcap: a clear rod seen against the studio, looked up by the view
 * space normal. Each texel traces the ray through a round cross-section in
 * the plane of the normal and the line of sight, the same four ways out as
 * the original, and keeps apart what comes from the studio (rgb) and how
 * much of the picture straight behind shows through (alpha), which the blend
 * adds back from the framebuffer.
 * ------------------------------------------------------------------------- */

#define MATCAP_SIZE 64
#define MATCAP_LODS 4
#define PAGE_SHOW   0.25f	// how much of the white page a rod shows through it
// A rod is only a few pixels across, so the original reads the studio blurred
// over each pixel's spread of directions: about this many radians
#define ROD_SPREAD  0.35f
static u8 *matcapData;
static GXTexObj matcapTex;

typedef struct { float s, z; } v2;

static bool refract2(v2 i, v2 n, float eta, v2 *out)
{
	float d = n.s * i.s + n.z * i.z;
	float k = 1.0f - eta * eta * (1.0f - d * d);
	if (k < 0.0f) return false;
	float f = eta * d + sqrtf(k);
	out->s = eta * i.s - f * n.s;
	out->z = eta * i.z - f * n.z;
	return true;
}

// The room averaged over a cone of directions round d
#define BLUR_TAPS 12
static float blurTaps[BLUR_TAPS][2];
static void initBlurTaps(void)
{
	for (int i = 0; i < BLUR_TAPS; i++) {
		float r = sqrtf((i + 0.5f) / BLUR_TAPS), a = i * 2.39996323f;
		blurTaps[i][0] = r * cosf(a);
		blurTaps[i][1] = r * sinf(a);
	}
}

// The studio's small hot lamps, far brighter than the room: a clear rod's rounded
// surface catches each as a pin-point. Blurred over a cone they spread their light
// (in the original's cube map, mip-mapped) over everything within it.
static const struct { float x, y; v3 col; bool front; } studioLamps[] = {
	{-0.42f, 0.98f, {1.0f, 1.1f, 1.27f}, true}, {0.62f, 0.72f, {0.85f, 0.96f, 1.1f}, true},
	{-0.95f, -0.5f, {0.75f, 0.85f, 1.0f}, true}, {0.85f, -0.7f, {0.75f, 0.85f, 1.0f}, true},
	{-0.62f, 0.58f, {0.15f, 0.17f, 0.2f}, false}, {0.66f, -0.6f, {0.15f, 0.17f, 0.2f}, false},
};
#define LAMP 12000.0f

static v3 lampsBlur(v3 d, float radius)
{
	v3 acc = V3(0, 0, 0);
	for (u32 i = 0; i < sizeof(studioLamps) / sizeof(studioLamps[0]); i++) {
		float z = studioLamps[i].front ? 1.0f : -1.0f;
		v3 l = vnorm(V3(studioLamps[i].x, studioLamps[i].y, z));
		float ang = acosf(clampf(vdot(d, l), -1.0f, 1.0f));
		if (ang > radius) continue;
		// a lamp is a disc of radius ~0.0075 on the plane z = 1: its solid angle shrinks off axis
		float cosz = fabsf(l.z), omega = PI_F * 0.0075f * 0.0075f * cosz * cosz * cosz;
		float cone = PI_F * radius * radius;
		float k = omega * LAMP / cone * (1.0f - smoothstep(0.7f * radius, radius, ang)) * smoothstep(0.0f, 0.2f, fabsf(l.z));
		acc = vadd(acc, vmul(studioLamps[i].col, k));
	}
	return acc;
}

static v3 roomBlur(v3 d, float radius)
{
	d = vnorm(d);
	v3 up = fabsf(d.z) < 0.9f ? V3(0, 0, 1) : V3(1, 0, 0);
	v3 t = vnorm(V3(up.y * d.z - up.z * d.y, up.z * d.x - up.x * d.z, up.x * d.y - up.y * d.x));
	v3 b = V3(d.y * t.z - d.z * t.y, d.z * t.x - d.x * t.z, d.x * t.y - d.y * t.x);
	v3 acc = V3(0, 0, 0);
	for (int i = 0; i < BLUR_TAPS; i++) {
		v3 dir = vadd(d, vadd(vmul(t, blurTaps[i][0] * radius), vmul(b, blurTaps[i][1] * radius)));
		acc = vadd(acc, room(dir));
	}
	return vadd(vmul(acc, 1.0f / BLUR_TAPS), lampsBlur(d, radius));
}

// A ray leaving the rod: what the studio gives (a) and the share of the picture behind (b).
// The backdrop fills about 15 degrees round the line of sight, widened by the spread
static void outsideRay(v3 dir, v3 *a, float *b)
{
	*a = roomBlur(dir, ROD_SPREAD);
	*b = 0.0f;
	if (dir.z > -0.05f) return;
	float spread = fminf(ROD_SPREAD, 0.3f);
	float bend = acosf(clampf(-dir.z, -1.0f, 1.0f));
	float w = 1.0f - smoothstep(0.04f - spread, 0.13f + spread, bend);
	*a = vmul(*a, 1.0f - w * PAGE_SHOW);
	*b = w * PAGE_SHOW;
}

static void matcapTexel(float nx, float ny, v3 *outCol, float *outB)
{
	float a = sqrtf(nx * nx + ny * ny);
	if (a > 0.999f) { nx *= 0.999f / a; ny *= 0.999f / a; a = 0.999f; }
	float b = sqrtf(1.0f - a * a);
	float ux = a > 1e-4f ? nx / a : 1.0f, uy = a > 1e-4f ? ny / a : 0.0f;
	#define TO3(v) V3((v).s * ux, (v).s * uy, (v).z)

	v2 N = {a, b}, I = {0.0f, -1.0f};
	float F = fresnel(b, IOR);
	v2 R = {2.0f * a * b, 2.0f * b * b - 1.0f};
	v3 R3 = TO3(R);
	v3 refl = vmix(roomBlur(R3, ROD_SPREAD), V3(1, 1, 1), backdropOf(R3));

	v3 acc = V3(0, 0, 0);
	float accB = 0.0f, thr = 1.0f;
	v2 dir, pos = N;
	if (!refract2(I, N, 1.0f / IOR, &dir)) dir = I;
	for (int k = 0; k < 4; k++) {
		float t = -2.0f * (pos.s * dir.s + pos.z * dir.z);
		pos.s += dir.s * t;
		pos.z += dir.z * t;
		v2 ne = pos, nin = {-pos.s, -pos.z}, out;
		bool trapped = !refract2(dir, nin, IOR, &out);
		float Fe = trapped ? 1.0f : fresnel(fabsf(out.s * ne.s + out.z * ne.z), IOR);
		if (!trapped) {
			v3 ea; float eb;
			outsideRay(vnorm(TO3(out)), &ea, &eb);
			float w = thr * (1.0f - Fe);
			acc = vadd(acc, vmul(ea, w));
			accB += eb * w;
		}
		thr *= Fe;
		float d = dir.s * ne.s + dir.z * ne.z;
		dir.s -= 2.0f * d * ne.s;
		dir.z -= 2.0f * d * ne.z;
	}
	// what is still inside leaks out dimly, as after many more bounces
	acc = vadd(acc, vmul(roomBlur(TO3(dir), ROD_SPREAD), thr * 0.6f));
	#undef TO3

	*outCol = vadd(vmul(refl, F), vmul(acc, 1.0f - F));
	*outB = accB * (1.0f - F);
}

static void putRGBA8(u8 *tex, int w, int x, int y, u8 r, u8 g, u8 b, u8 a)
{
	int tile = (y >> 2) * (w >> 2) + (x >> 2);
	int off = tile * 64 + ((y & 3) * 4 + (x & 3)) * 2;
	tex[off] = a;
	tex[off + 1] = r;
	tex[off + 32] = g;
	tex[off + 33] = b;
}

static void getRGBA8(const u8 *tex, int w, int x, int y, int *rgba)
{
	int tile = (y >> 2) * (w >> 2) + (x >> 2);
	int off = tile * 64 + ((y & 3) * 4 + (x & 3)) * 2;
	rgba[3] += tex[off];
	rgba[0] += tex[off + 1];
	rgba[1] += tex[off + 32];
	rgba[2] += tex[off + 33];
}

static u32 rgba8Size(int w, int h) { return (u32)(((w + 3) & ~3) * ((h + 3) & ~3) * 4); }

// Box filter one RGBA8 level down into the next
static void downsampleRGBA8(const u8 *src, int w, int h, u8 *dst)
{
	int dw = w / 2, dh = h / 2;
	for (int y = 0; y < dh; y++) {
		for (int x = 0; x < dw; x++) {
			int c[4] = {0, 0, 0, 0};
			getRGBA8(src, w, x * 2, y * 2, c);
			getRGBA8(src, w, x * 2 + 1, y * 2, c);
			getRGBA8(src, w, x * 2, y * 2 + 1, c);
			getRGBA8(src, w, x * 2 + 1, y * 2 + 1, c);
			putRGBA8(dst, dw, x, y, c[0] >> 2, c[1] >> 2, c[2] >> 2, c[3] >> 2);
		}
	}
}

static void buildMatcap(void)
{
	u32 total = 0;
	for (int l = 0, s = MATCAP_SIZE; l < MATCAP_LODS; l++, s >>= 1) total += rgba8Size(s, s);
	initBlurTaps();
	matcapData = memalign(32, total);
	for (int y = 0; y < MATCAP_SIZE; y++) {
		for (int x = 0; x < MATCAP_SIZE; x++) {
			float nx = (x + 0.5f) / (MATCAP_SIZE / 2) - 1.0f;
			float ny = 1.0f - (y + 0.5f) / (MATCAP_SIZE / 2);
			v3 col; float B;
			matcapTexel(nx, ny, &col, &B);
			// Blended as src + dst * B: exact over the white page
			float r = toSrgb(col.x + B) - B, g = toSrgb(col.y + B) - B, b = toSrgb(col.z + B) - B;
			putRGBA8(matcapData, MATCAP_SIZE, x, y, (u8)(sat(r) * 255.0f), (u8)(sat(g) * 255.0f),
				(u8)(sat(b) * 255.0f), (u8)(sat(B) * 255.0f));
		}
	}
	u8 *level = matcapData;
	for (int l = 1, s = MATCAP_SIZE; l < MATCAP_LODS; l++, s >>= 1) {
		u8 *next = level + rgba8Size(s, s);
		downsampleRGBA8(level, s, s, next);
		level = next;
	}
	DCFlushRange(matcapData, total);
	GX_InitTexObj(&matcapTex, matcapData, MATCAP_SIZE, MATCAP_SIZE, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_TRUE);
	GX_InitTexObjLOD(&matcapTex, GX_LIN_MIP_LIN, GX_LINEAR, 0.0f, MATCAP_LODS - 1, 0.0f, GX_DISABLE, GX_ENABLE, GX_ANISO_1);
}

/* ---------------------------------------------------------------------------
 * Paths, as the original builds them (2D, millimetres)
 * ------------------------------------------------------------------------- */

#define PATH_MAX_PTS 512
#define DETAIL 2.0f		// the original's point spacing is multiplied by this

typedef struct { float p[PATH_MAX_PTS][2]; int n; } path_t;

static void pathPush(path_t *o, float x, float y)
{
	if (o->n < PATH_MAX_PTS) { o->p[o->n][0] = x; o->p[o->n][1] = y; o->n++; }
}

// Points enough round an arc of radius r for the chords to keep within
// ARC_TOL of it (a fifth of a pixel at the case's usual size), and no more than
// the original's spacing gives
#define ARC_TOL 0.1f
static int arcPoints(float r, float angle, float step)
{
	int n = (int)ceilf(angle * r / (step * DETAIL));
	if (r > ARC_TOL) {
		int byTol = (int)ceilf(angle / (2.0f * acosf(1.0f - ARC_TOL / r)));
		if (byTol < n) n = byTol;
	}
	return n;
}

static void arcPts(path_t *o, float cx, float cy, float r, float a0, float a1, float step)
{
	int n = arcPoints(r, fabsf(a1 - a0), step);
	if (n < 2) n = 2;
	for (int i = 0; i <= n; i++) {
		float a = a0 + (a1 - a0) * i / n;
		pathPush(o, cx + r * cosf(a), cy + r * sinf(a));
	}
}

// Round off the corners of a polyline with arcs of radius t
static void roundCorners(const float (*e)[2], int n, float t, bool closed, path_t *o)
{
	o->n = 0;
	for (int i = 0; i < n; i++) {
		const float *pt = e[i];
		if (!closed && (i == 0 || i == n - 1)) { pathPush(o, pt[0], pt[1]); continue; }
		const float *s = e[(i - 1 + n) % n], *l = e[(i + 1) % n];
		float cx = s[0] - pt[0], cy = s[1] - pt[1], cl = sqrtf(cx * cx + cy * cy);
		float dx = l[0] - pt[0], dy = l[1] - pt[1], dl = sqrtf(dx * dx + dy * dy);
		cx /= cl; cy /= cl; dx /= dl; dy /= dl;
		float h = acosf(clampf(cx * dx + cy * dy, -1.0f, 1.0f)) / 2.0f;
		if (h > PI_F / 2.0f - 0.001f) { pathPush(o, pt[0], pt[1]); continue; }
		float u = t / tanf(h);
		float vx = cx + dx, vy = cy + dy, vl = sqrtf(vx * vx + vy * vy);
		vx /= vl; vy /= vl;
		float mx = pt[0] + vx * (t / sinf(h)), my = pt[1] + vy * (t / sinf(h));
		float gx = pt[0] + cx * u, gy = pt[1] + cy * u;
		float xx = pt[0] + dx * u, xy = pt[1] + dy * u;
		float y = atan2f(gy - my, gx - mx), b = atan2f(xy - my, xx - mx);
		if (b - y > PI_F) b -= 2.0f * PI_F;
		if (y - b > PI_F) b += 2.0f * PI_F;
		arcPts(o, mx, my, t, y, b, 0.8f);
	}
}

static void roundSquare(float e, float t, path_t *o)
{
	const float q[4][2] = {{e, -e}, {e, e}, {-e, e}, {-e, -e}};
	roundCorners(q, 4, t, true, o);
}

static void roundRect(float cx, float cy, float hw, float hh, float r, path_t *o)
{
	const float q[4][2] = {{cx + hw, cy - hh}, {cx + hw, cy + hh}, {cx - hw, cy + hh}, {cx - hw, cy - hh}};
	roundCorners(q, 4, r, true, o);
}

static void circlePts(float cx, float cy, float r, float step, path_t *o)
{
	o->n = 0;
	int n = arcPoints(r, 2.0f * PI_F, step);
	if (n < 12) n = 12;
	for (int i = 0; i < n; i++) {
		float a = (float)i / n * 2.0f * PI_F;
		pathPush(o, cx + r * cosf(a), cy + r * sinf(a));
	}
}

static void segment(float x0, float y0, float x1, float y1, path_t *o)
{
	o->n = 0;
	pathPush(o, x0, y0);
	pathPush(o, x1, y1);
}

/* ---------------------------------------------------------------------------
 * Rods: a rounded rectangle profile (half width hw across, half height hh in
 * z, corner radius rc, centred at height zc) swept along a path. Open paths
 * end in a dome, or flat when asked (the hinge knuckles).
 * ------------------------------------------------------------------------- */

typedef struct { float hw, hh, rc, zc; int seg; bool flat; } profile_t;

typedef struct {
	float *pos, *nrm;
	int count, cap;
	int glintEnd;	// the first rod, the frame, is where the lamps glint
	u16 *prims;		// [GX primitive, count, indices...]...
	int primLen, primCap;
	void *dl;
	u32 dlSize;
} rodmesh_t;

static int rodVertex(rodmesh_t *m, v3 p, v3 n)
{
	if (m->count == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 1024;
		m->pos = realloc(m->pos, m->cap * 3 * sizeof(float));
		m->nrm = realloc(m->nrm, m->cap * 3 * sizeof(float));
	}
	n = vnorm(n);
	float *pp = m->pos + m->count * 3, *nn = m->nrm + m->count * 3;
	pp[0] = p.x; pp[1] = p.y; pp[2] = p.z;
	nn[0] = n.x; nn[1] = n.y; nn[2] = n.z;
	return m->count++;
}

static void rodPrim(rodmesh_t *m, u8 type, const u16 *idx, int n)
{
	if (m->primLen + n + 2 > m->primCap) {
		m->primCap = (m->primLen + n + 2) * 2;
		m->prims = realloc(m->prims, m->primCap * sizeof(u16));
	}
	m->prims[m->primLen++] = type;
	m->prims[m->primLen++] = n;
	memcpy(m->prims + m->primLen, idx, n * sizeof(u16));
	m->primLen += n;
}

static void rodAdd(rodmesh_t *m, const path_t *path, profile_t pr, bool closed)
{
	// the profile round its four corners
	int seg = (pr.seg ? pr.seg : 6);
	seg = (seg + 1) / 2;
	// the thinnest are a pixel or two across: eight sides are as round as they look
	if (pr.hw < 1.0f && pr.hh < 1.0f) seg = 1;
	float pts[40][2], nrm[40][2];
	int np = 0;
	const float corners[4][3] = {
		{pr.hw - pr.rc, pr.hh - pr.rc, 0.0f}, {-(pr.hw - pr.rc), pr.hh - pr.rc, PI_F / 2.0f},
		{-(pr.hw - pr.rc), -(pr.hh - pr.rc), PI_F}, {pr.hw - pr.rc, -(pr.hh - pr.rc), 3.0f * PI_F / 2.0f}};
	for (int c = 0; c < 4; c++) {
		for (int t = 0; t <= seg; t++) {
			float a = corners[c][2] + (float)t / seg * (PI_F / 2.0f);
			float x = corners[c][0] + pr.rc * cosf(a), y = corners[c][1] + pr.rc * sinf(a);
			if (np && hypotf(x - pts[np - 1][0], y - pts[np - 1][1]) < 1e-6f) continue;
			pts[np][0] = x; pts[np][1] = y;
			nrm[np][0] = cosf(a); nrm[np][1] = sinf(a);
			np++;
		}
	}
	if (np > 1 && hypotf(pts[0][0] - pts[np - 1][0], pts[0][1] - pts[np - 1][1]) < 1e-6f) np--;

	int s = path->n;
	float tx[PATH_MAX_PTS], ty[PATH_MAX_PTS];
	for (int i = 0; i < s; i++) {
		const float *a = path->p[closed ? (i - 1 + s) % s : (i > 0 ? i - 1 : 0)];
		const float *b = path->p[closed ? (i + 1) % s : (i < s - 1 ? i + 1 : s - 1)];
		float dx = b[0] - a[0], dy = b[1] - a[1], l = hypotf(dx, dy);
		if (l < 1e-9f) l = 1.0f;
		tx[i] = dx / l; ty[i] = dy / l;
	}
	int base = m->count;
	for (int i = 0; i < s; i++) {
		float sx = -ty[i], sy = tx[i];
		for (int j = 0; j < np; j++) {
			rodVertex(m, V3(path->p[i][0] + sx * pts[j][0], path->p[i][1] + sy * pts[j][0], pr.zc + pts[j][1]),
				V3(sx * nrm[j][0], sy * nrm[j][0], nrm[j][1]));
		}
	}
	u16 strip[2 * 41];
	int segs = closed ? s : s - 1;
	for (int i = 0; i < segs; i++) {
		int r0 = base + i * np, r1 = base + ((i + 1) % s) * np, k = 0;
		for (int j = 0; j <= np; j++) {
			strip[k++] = r0 + j % np;
			strip[k++] = r1 + j % np;
		}
		rodPrim(m, GX_TRIANGLESTRIP, strip, k);
	}
	if (closed) return;

	for (int end = 0; end < 2; end++) {
		int i = end ? s - 1 : 0;
		float sign = end ? 1.0f : -1.0f;
		float sx = -ty[i], sy = tx[i];
		v3 h = V3(path->p[i][0], path->p[i][1], pr.zc);
		v3 u = V3(tx[i] * sign, ty[i] * sign, 0.0f);
		if (pr.flat) {
			u16 fan[42];
			int k = 0;
			fan[k++] = rodVertex(m, h, u);
			for (int j = 0; j <= np; j++) {
				int jj = j % np;
				fan[k++] = rodVertex(m, V3(h.x + sx * pts[jj][0], h.y + sy * pts[jj][0], pr.zc + pts[jj][1]), u);
			}
			rodPrim(m, GX_TRIANGLEFAN, fan, k);
			continue;
		}
		float e = 0.8f * fminf(pr.hw, pr.hh);
		int prev = base + i * np;
		for (int n = 1; n <= 3; n++) {
			float c = n / 3.0f * (PI_F / 2.0f), d = cosf(c), v = sinf(c) * e;
			int ring = m->count;
			for (int j = 0; j < np; j++) {
				v3 mm = V3(sx * nrm[j][0], sy * nrm[j][0], nrm[j][1]);
				v3 g = V3(mm.x * d * e + u.x * sinf(c) * e * 1.1f, mm.y * d * e + u.y * sinf(c) * e * 1.1f, mm.z * d * e);
				rodVertex(m, V3(h.x + sx * pts[j][0] * d + u.x * v, h.y + sy * pts[j][0] * d + u.y * v, pr.zc + pts[j][1] * d), g);
			}
			int k = 0;
			for (int j = 0; j <= np; j++) {
				strip[k++] = prev + j % np;
				strip[k++] = ring + j % np;
			}
			rodPrim(m, GX_TRIANGLESTRIP, strip, k);
			prev = ring;
		}
	}
}

static void rodSetupVtx(void)
{
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
	GX_SetVtxDesc(GX_VA_NRM, GX_INDEX16);
	GX_SetVtxAttrFmt(VTXFMT_ROD, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(VTXFMT_ROD, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
}

// Move the arrays to aligned memory and record the strips in a display list
static void rodCompile(rodmesh_t *m)
{
	float *pos = memalign(32, m->count * 12), *nrm = memalign(32, m->count * 12);
	memcpy(pos, m->pos, m->count * 12);
	memcpy(nrm, m->nrm, m->count * 12);
	free(m->pos);
	free(m->nrm);
	m->pos = pos;
	m->nrm = nrm;
	DCFlushRange(pos, m->count * 12);
	DCFlushRange(nrm, m->count * 12);

	u32 size = 64;
	for (int i = 0; i < m->primLen; i += m->prims[i + 1] + 2) size += 3 + m->prims[i + 1] * 4;
	size = (size + 31) & ~31;
	m->dl = memalign(32, size);
	memset(m->dl, 0, size);
	DCInvalidateRange(m->dl, size);
	rodSetupVtx();
	GX_BeginDispList(m->dl, size);
	for (int i = 0; i < m->primLen; i += m->prims[i + 1] + 2) {
		int n = m->prims[i + 1];
		GX_Begin(m->prims[i], VTXFMT_ROD, n);
		for (int j = 0; j < n; j++) {
			GX_Position1x16(m->prims[i + 2 + j]);
			GX_Normal1x16(m->prims[i + 2 + j]);
		}
		GX_End();
	}
	m->dlSize = GX_EndDispList();
	free(m->prims);
	m->prims = NULL;
}

static rodmesh_t trayRods, lidRods;

// The hinge axis the lid turns about
#define HINGE_X -72.0f
#define HINGE_Z 4.0f

static const profile_t PROF_FRAME = {3.3f, 3.1f, 2.8f, -0.7f, 6, false};
static const profile_t PROF_RAIL  = {2.4f, 2.6f, 2.2f, -1.2f, 6, false};

static void buildTrayRods(rodmesh_t *m)
{
	path_t p;
	float r = 73.0f - PROF_FRAME.hw;
	// the frame, and the lip round the back plate
	roundSquare(r, 6.5f, &p);
	rodAdd(m, &p, PROF_FRAME, true);
	m->glintEnd = m->count;
	roundSquare(72.25f, 7.4f, &p);
	rodAdd(m, &p, (profile_t) {0.75f, 0.7f, 0.68f, -4.5f, 6, false}, true);

	// the bumps along the frame
	profile_t bump = {3.75f, 3.5f, 3.1f, PROF_FRAME.zc, 6, false};
	const float at[4] = {-35.0f, -12.0f, 12.0f, 35.0f};
	for (int e = -1; e <= 1; e += 2) {
		for (int k = 0; k < 4; k++) {
			segment(at[k] - 2.2f, e * r, at[k] + 2.2f, e * r, &p);
			rodAdd(m, &p, bump, false);
		}
		for (int k = 0; k < 4; k++) {
			if (e < 0 && fabsf(at[k]) < 20.0f) continue;
			segment(e * r, at[k] - 2.2f, e * r, at[k] + 2.2f, &p);
			rodAdd(m, &p, bump, false);
		}
	}

	// the rails that hold the disc in the corners
	const float o = 63.4f;
	const int corners[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
	profile_t foot = {1.9f, 2.2f, 1.6f, PROF_RAIL.zc, 6, false};
	for (int c = 0; c < 4; c++) {
		float e = corners[c][0], t = corners[c][1];
		float a = e < 0 ? 26.0f : 35.0f;
		const float q[3][2] = {{e * o, t * a}, {e * o, t * o}, {35.0f * e, t * o}};
		roundCorners(q, 3, 4.5f, false, &p);
		rodAdd(m, &p, PROF_RAIL, false);
		segment(e * o, t * a, e * r, t * a, &p);
		rodAdd(m, &p, foot, false);
		segment(35.0f * e, t * o, 35.0f * e, t * r, &p);
		rodAdd(m, &p, foot, false);
	}

	// the ring round the disc, and its four tabs
	circlePts(0, 0, 61.6f, 1.1f, &p);
	rodAdd(m, &p, (profile_t) {1.15f, 1.7f, 1.0f, -2.1f, 5, false}, true);
	circlePts(0, 0, 61.6f - 1.55f, 1.1f, &p);
	rodAdd(m, &p, (profile_t) {0.5f, 0.8f, 0.45f, -3.0f, 4, false}, true);
	const float tabs[4] = {59.0f, 121.0f, 238.5f, 301.5f};
	for (int k = 0; k < 4; k++) {
		float t = tabs[k] * PI_F / 180.0f;
		segment(58.6f * cosf(t), 58.6f * sinf(t), 63.2f * cosf(t), 63.2f * sinf(t), &p);
		rodAdd(m, &p, (profile_t) {1.6f, 1.0f, 0.85f, -0.6f, 4, false}, false);
	}

	// the finger notches along the bottom
	for (int e = -1; e <= 1; e += 2) {
		const float q[3][2] = {{46.0f * e, -o}, {46.0f * e, -49.5f}, {42.6f * e, -45.6f}};
		roundCorners(q, 3, 4.2f, false, &p);
		rodAdd(m, &p, (profile_t) {1.9f, 1.9f, 1.8f, -2.0f, 6, false}, false);
	}

	// the tab by the spine
	float sx = (-73.0f + -50.5f) / 2.0f, cx = (-50.5f - -73.0f) / 2.0f;
	roundRect(sx, 0, cx - 0.8f, 22.0f - 0.8f, 3.4f, &p);
	rodAdd(m, &p, (profile_t) {0.8f, 0.8f, 0.75f, 1.8f, 4, false}, true);
	{
		const float q[4][2] = {{-69.0f, 13.0f}, {-58.5f, 13.0f}, {-58.5f, -13.0f}, {-69.0f, -13.0f}};
		roundCorners(q, 4, 3.2f, false, &p);
		rodAdd(m, &p, (profile_t) {1.35f, 1.0f, 0.9f, 3.0f, 6, false}, false);
	}
	roundRect(-55.4f, 0, 4.4f, 3.2f, 1.2f, &p);
	rodAdd(m, &p, (profile_t) {0.65f, 0.55f, 0.5f, 3.0f, 3, false}, true);

	// the hub the disc clips onto
	circlePts(0, 0, 18.8f, 0.7f, &p);
	rodAdd(m, &p, (profile_t) {0.8f, 0.75f, 0.7f, -0.5f, 4, false}, true);
	circlePts(0, 0, 16.5f, 0.7f, &p);
	rodAdd(m, &p, (profile_t) {0.45f, 0.45f, 0.42f, -0.15f, 3, false}, true);
	circlePts(0, 0, 9.6f, 0.6f, &p);
	rodAdd(m, &p, (profile_t) {0.95f, 0.95f, 0.9f, 0.0f, 4, false}, true);
	circlePts(0, 0, 6.6f, 0.5f, &p);
	rodAdd(m, &p, (profile_t) {0.55f, 0.85f, 0.5f, -0.35f, 3, false}, true);
	for (int c = 0; c < 4; c++) {
		float e = c == 0 || c == 3 ? 1.0f : -1.0f, t = c < 2 ? 1.0f : -1.0f;
		circlePts(8.9f * e, 8.9f * t, 2.0f, 0.4f, &p);
		rodAdd(m, &p, (profile_t) {0.45f, 0.6f, 0.42f, -0.3f, 3, false}, true);
	}
	// the clear middle round the disc's hole
	circlePts(0, 0, 7.75f, 0.5f, &p);
	rodAdd(m, &p, (profile_t) {0.25f, 0.6f, 0.24f, -1.8f, 3, false}, true);

	// the hinge knuckles
	profile_t knuckle = {2.1f, 2.1f, 2.08f, HINGE_Z, 6, true};
	segment(HINGE_X, 44.0f, HINGE_X, 63.0f, &p);
	rodAdd(m, &p, knuckle, false);
	segment(HINGE_X, -63.0f, HINGE_X, -44.0f, &p);
	rodAdd(m, &p, knuckle, false);
}

static void buildLidRods(rodmesh_t *m)
{
	path_t p;
	roundSquare(72.0f, 7.0f, &p);
	rodAdd(m, &p, (profile_t) {1.0f, 1.0f, 0.98f, 4.0f, 5, false}, true);
	m->glintEnd = m->count;
	roundSquare(69.6f, 5.0f, &p);
	rodAdd(m, &p, (profile_t) {0.42f, 0.4f, 0.38f, 3.25f, 3, false}, true);
	const float notches[2] = {-42.0f, 30.0f};
	for (int t = -1; t <= 1; t += 2) {
		for (int k = 0; k < 2; k++) {
			p.n = 0;
			arcPts(&p, notches[k], 69.6f * t, 4.4f, t > 0 ? PI_F : 0.0f, t > 0 ? 2.0f * PI_F : PI_F, 0.5f);
			rodAdd(m, &p, (profile_t) {0.42f, 0.38f, 0.36f, 3.22f, 3, false}, false);
		}
	}
	for (int t = -1; t <= 1; t += 2) {
		for (int n = -30; n <= 30; n += 60) {
			segment(69.6f * t, n - 3.0f, 69.6f * t, n + 3.0f, &p);
			rodAdd(m, &p, (profile_t) {0.5f, 0.45f, 0.4f, 3.3f, 3, false}, false);
		}
	}
	profile_t knuckle = {2.0f, 2.0f, 1.98f, HINGE_Z, 6, true};
	segment(HINGE_X, 20.0f, HINGE_X, 42.0f, &p);
	rodAdd(m, &p, knuckle, false);
	segment(HINGE_X, -42.0f, HINGE_X, -20.0f, &p);
	rodAdd(m, &p, knuckle, false);
}

/* ---------------------------------------------------------------------------
 * Sheets: the flat clear parts, two faces each. Rings run from the outline in
 * to the middle (or a hole) so the light they mirror can vary across them.
 * ------------------------------------------------------------------------- */

#define SHEET_RINGS 4

typedef struct {
	int segs;			// points round the outline
	float *xy;			// (SHEET_RINGS + 1) x segs points
	float z0, z1;
} sheet_t;

static sheet_t traySheets[4], lidSheet;
static int traySheetCount;

static void sheetBuild(sheet_t *s, const path_t *outer, const path_t *inner, float cx, float cy, float z0, float z1)
{
	s->segs = outer->n;
	s->z0 = z0;
	s->z1 = z1;
	s->xy = malloc((SHEET_RINGS + 1) * s->segs * 2 * sizeof(float));
	for (int r = 0; r <= SHEET_RINGS; r++) {
		float t = (float)r / SHEET_RINGS;
		for (int i = 0; i < s->segs; i++) {
			float ix = inner ? inner->p[i][0] : cx, iy = inner ? inner->p[i][1] : cy;
			s->xy[(r * s->segs + i) * 2 + 0] = mixf(outer->p[i][0], ix, t);
			s->xy[(r * s->segs + i) * 2 + 1] = mixf(outer->p[i][1], iy, t);
		}
	}
}

static void buildSheets(void)
{
	path_t o, in;
	traySheetCount = 0;
	roundSquare(73.0f, 8.0f, &o);
	sheetBuild(&traySheets[traySheetCount++], &o, NULL, 0, 0, -5.2f, -3.8f);
	float sx = (-73.0f + -50.5f) / 2.0f, cx = (-50.5f - -73.0f) / 2.0f;
	roundRect(sx, 0, cx, 22.0f, 4.2f, &o);
	sheetBuild(&traySheets[traySheetCount++], &o, NULL, sx, 0, 1.0f, 2.6f);
	circlePts(0, 0, 19.4f, 1.4f, &o);
	{
		// the hole, with as many points as the outline
		in.n = 0;
		for (int i = 0; i < o.n; i++) {
			float a = (float)i / o.n * 2.0f * PI_F;
			pathPush(&in, 6.2f * cosf(a), 6.2f * sinf(a));
		}
	}
	sheetBuild(&traySheets[traySheetCount++], &o, &in, 0, 0, -1.1f, -0.2f);
	circlePts(0, 0, 15.0f, 1.2f, &o);
	in.n = 0;
	for (int i = 0; i < o.n; i++) {
		float a = (float)i / o.n * 2.0f * PI_F;
		pathPush(&in, 7.5f * cosf(a), 7.5f * sinf(a));
	}
	sheetBuild(&traySheets[traySheetCount++], &o, &in, 0, 0, -2.4f, -1.2f);
	roundSquare(73.0f, 8.0f, &o);
	sheetBuild(&lidSheet, &o, NULL, 0, 0, 3.0f, 5.0f);
}

/* ---------------------------------------------------------------------------
 * Studio set-up shared by the draw code
 * ------------------------------------------------------------------------- */

static const v3 LIGHT_DIR = {-0.3f, 0.48f, 1.0f};	// the light that casts the shadows (world)
#define PAGE_Z -70.0f
#define FOV_Y 28.0f
#define CASE_HALF 73.0f

static float camDist;
static Mtx view;

/* ---------------------------------------------------------------------------
 * Disc art cache
 * ------------------------------------------------------------------------- */

#define ART_SLOTS 16
#define ART_THUMB 64

typedef struct {
	char id[JEWEL_ID_LEN];
	bool used, missing;
	int w, h;
	u8 *tex, *thumb;
	GXTexObj texObj, thumbObj;
	u32 lastUse;
} art_t;

static art_t art[ART_SLOTS];
static mutex_t jcMutex = LWP_MUTEX_NULL;
static u32 frameNo;

static art_t *findArt(const char *id)
{
	if (!id[0]) return NULL;
	for (int i = 0; i < ART_SLOTS; i++) {
		if (art[i].used && !strncmp(art[i].id, id, JEWEL_ID_LEN)) return &art[i];
	}
	return NULL;
}

static void artLock(void)
{
	if (jcMutex == LWP_MUTEX_NULL) LWP_MutexInit(&jcMutex, false);
	LWP_MutexLock(jcMutex);
}

static void artUnlock(void)
{
	LWP_MutexUnlock(jcMutex);
}

void JewelCase_GameId(file_handle *file, char *id)
{
	memset(id, 0, JEWEL_ID_LEN);
	if (!file || !file->meta) return;
	const char *g = file->meta->diskId.gamename, *c = file->meta->diskId.company;
	for (int i = 0; i < 4; i++) if (g[i] < 0x20 || g[i] > 0x7E) return;
	for (int i = 0; i < 2; i++) if (c[i] < 0x20 || c[i] > 0x7E) return;
	memcpy(id, g, 4);
	memcpy(id + 4, c, 2);
}

bool JewelCase_NeedsArt(const char *id)
{
	if (!id[0]) return false;
	artLock();
	bool need = findArt(id) == NULL;
	artUnlock();
	return need;
}

// An RGBA image into a tiled RGBA8 texture, box filtered when it shrinks. In
// whole numbers: it runs on the main thread while the case is drawn
static void toTexture(const u8 *src, int sw, int sh, u8 *dst, int dw, int dh)
{
	if (sw == dw && sh == dh) {
		for (int y = 0; y < dh; y++) {
			const u8 *p = src + y * sw * 4;
			for (int x = 0; x < dw; x++, p += 4) putRGBA8(dst, dw, x, y, p[0], p[1], p[2], p[3]);
		}
		return;
	}
	for (int y = 0; y < dh; y++) {
		int iy0 = y * sh / dh, iy1 = ((y + 1) * sh + dh - 1) / dh;
		if (iy1 <= iy0) iy1 = iy0 + 1;
		if (iy1 > sh) iy1 = sh;
		for (int x = 0; x < dw; x++) {
			int ix0 = x * sw / dw, ix1 = ((x + 1) * sw + dw - 1) / dw;
			if (ix1 <= ix0) ix1 = ix0 + 1;
			if (ix1 > sw) ix1 = sw;
			u32 acc[4] = {0, 0, 0, 0}, n = 0;
			for (int sy = iy0; sy < iy1; sy++) {
				const u8 *p = src + (sy * sw + ix0) * 4;
				for (int sx = ix0; sx < ix1; sx++, p += 4) {
					// weight colour by alpha so the clear edge doesn't darken it
					acc[0] += p[0] * p[3]; acc[1] += p[1] * p[3]; acc[2] += p[2] * p[3]; acc[3] += p[3];
					n++;
				}
			}
			u8 a = n ? acc[3] / n : 0;
			u8 r = acc[3] ? acc[0] / acc[3] : 0, g = acc[3] ? acc[1] / acc[3] : 0, b = acc[3] ? acc[2] / acc[3] : 0;
			putRGBA8(dst, dw, x, y, r, g, b, a);
		}
	}
}

void JewelCase_LoadArt(const char *id)
{
	if (!id[0] || !devices[DEVICE_CUR]) return;
	u8 *rgba = NULL, *tex = NULL, *thumb = NULL;
	int w = 0, h = 0;

	file_handle *file = calloc(1, sizeof(file_handle));
	if (file) {
		char name[32];
		snprintf(name, sizeof(name), "swiss/discs/%.6s.png", id);
		concat_path(file->name, devices[DEVICE_CUR]->initial->name, name);
		if (!devices[DEVICE_CUR]->statFile(file) && file->size > 0 && file->size < 4 * 1024 * 1024) {
			u8 *buf = malloc(file->size);
			if (buf) {
				devices[DEVICE_CUR]->seekFile(file, 0, DEVICE_HANDLER_SEEK_SET);
				if (devices[DEVICE_CUR]->readFile(file, buf, file->size) == (s32)file->size) {
					rgba = PNG_Decode(buf, file->size, &w, &h);
				}
				free(buf);
			}
		}
		devices[DEVICE_CUR]->closeFile(file);
		free(file);
	}
	if (rgba) {
		// keep the disc at its own size (rounded to whole tiles), and a small one for the dock
		int tw = (w + 3) & ~3, th = (h + 3) & ~3;
		if (tw > 256) tw = 256;
		if (th > 256) th = 256;
		tex = memalign(32, rgba8Size(tw, th));
		thumb = memalign(32, rgba8Size(ART_THUMB, ART_THUMB));
		if (tex && thumb) {
			toTexture(rgba, w, h, tex, tw, th);
			toTexture(rgba, w, h, thumb, ART_THUMB, ART_THUMB);
			DCFlushRange(tex, rgba8Size(tw, th));
			DCFlushRange(thumb, rgba8Size(ART_THUMB, ART_THUMB));
			w = tw;
			h = th;
		}
		else {
			free(tex);
			free(thumb);
			tex = thumb = NULL;
		}
		free(rgba);
	}

	artLock();
	art_t *slot = findArt(id);
	if (!slot) {
		// a free slot, or the one drawn longest ago
		for (int i = 0; i < ART_SLOTS && !slot; i++) if (!art[i].used) slot = &art[i];
		if (!slot) {
			slot = &art[0];
			for (int i = 1; i < ART_SLOTS; i++) if (art[i].lastUse < slot->lastUse) slot = &art[i];
		}
		free(slot->tex);
		free(slot->thumb);
		memset(slot, 0, sizeof(*slot));
		strncpy(slot->id, id, JEWEL_ID_LEN - 1);
		slot->used = true;
		slot->lastUse = frameNo;
		slot->missing = tex == NULL;
		if (tex) {
			slot->tex = tex;
			slot->thumb = thumb;
			slot->w = w;
			slot->h = h;
			GX_InitTexObj(&slot->texObj, tex, w, h, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
			GX_InitTexObjLOD(&slot->texObj, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_DISABLE, GX_DISABLE, GX_ANISO_1);
			GX_InitTexObj(&slot->thumbObj, thumb, ART_THUMB, ART_THUMB, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
			GX_InitTexObjLOD(&slot->thumbObj, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_DISABLE, GX_DISABLE, GX_ANISO_1);
		}
	}
	else {
		free(tex);
		free(thumb);
	}
	artUnlock();
}

/* ---------------------------------------------------------------------------
 * Animation, as the original: curves are monotone cubic splines through the
 * keyframes, a case at rest sways a little, opening turns it round once as
 * the lid swings over and the two halves spread apart.
 * ------------------------------------------------------------------------- */

typedef struct { int n; float x[10], y[10], m[10]; } curve_t;

static void curveInit(curve_t *c, const float (*k)[2], int n)
{
	c->n = n;
	float d[10];
	for (int i = 0; i < n; i++) { c->x[i] = k[i][0]; c->y[i] = k[i][1]; c->m[i] = 0.0f; }
	for (int i = 0; i < n - 1; i++) d[i] = (c->y[i + 1] - c->y[i]) / (c->x[i + 1] - c->x[i]);
	for (int i = 1; i < n - 1; i++) c->m[i] = d[i - 1] * d[i] <= 0.0f ? 0.0f : (d[i - 1] + d[i]) / 2.0f;
	for (int i = 0; i < n - 1; i++) {
		if (d[i] == 0.0f) { c->m[i] = 0.0f; c->m[i + 1] = 0.0f; continue; }
		float a = c->m[i] / d[i], b = c->m[i + 1] / d[i], s = a * a + b * b;
		if (s > 9.0f) {
			float t = 3.0f / sqrtf(s);
			c->m[i] = t * a * d[i];
			c->m[i + 1] = t * b * d[i];
		}
	}
}

static float curveAt(const curve_t *c, float e)
{
	if (e <= c->x[0]) return c->y[0];
	if (e >= c->x[c->n - 1]) return c->y[c->n - 1];
	int r = 0;
	while (e > c->x[r + 1]) r++;
	float i = c->x[r + 1] - c->x[r], s = (e - c->x[r]) / i, l = s * s, cc = l * s;
	return (2 * cc - 3 * l + 1) * c->y[r] + (cc - 2 * l + s) * i * c->m[r] + (-2 * cc + 3 * l) * c->y[r + 1] +
		(cc - l) * i * c->m[r + 1];
}

static curve_t openYaw, openLid, openSpread, openPitch;
static curve_t closeLid, closeSpread, closeYaw, closePitch, closeRoll;

static void initCurves(void)
{
	const float oy[][2] = {{0, 0}, {.55f, .3f}, {1, .85f}, {1.2f, 2}, {1.41f, 3.4f}, {1.63f, 4.9f}, {1.85f, 6.05f}, {2.05f, 6.36f}, {2.3f, 2 * PI_F}};
	const float ol[][2] = {{0, 0}, {.97f, 0}, {1.2f, .45f}, {1.41f, 1.35f}, {1.63f, 2.3f}, {1.85f, 2.92f}, {2.05f, 3.12f}, {2.3f, 3.05f}};
	const float os[][2] = {{0, 0}, {1.12f, 0}, {1.9f, .9f}, {2.3f, 1}};
	const float op[][2] = {{0, 0}, {1.35f, 0}, {2.3f, -.1f}};
	const float cl[][2] = {{0, 3.05f}, {.22f, 1.75f}, {.48f, 1.55f}, {1.05f, 1.08f}, {1.42f, .6f}, {1.6f, .26f}, {1.7f, 0}, {1.78f, .07f}, {1.88f, 0}};
	const float cs[][2] = {{0, 1}, {.3f, .92f}, {1.2f, .55f}, {1.72f, 0}};
	const float cy[][2] = {{0, 0}, {.5f, .2f}, {1.3f, .12f}, {1.88f, 0}};
	const float cp[][2] = {{0, -.1f}, {1.6f, -.05f}, {1.7f, .035f}, {1.82f, -.01f}, {1.95f, 0}};
	const float cr[][2] = {{0, 0}, {.8f, -.05f}, {1.95f, 0}};
	curveInit(&openYaw, oy, 9);
	curveInit(&openLid, ol, 8);
	curveInit(&openSpread, os, 4);
	curveInit(&openPitch, op, 3);
	curveInit(&closeLid, cl, 9);
	curveInit(&closeSpread, cs, 4);
	curveInit(&closeYaw, cy, 4);
	curveInit(&closePitch, cp, 5);
	curveInit(&closeRoll, cr, 3);
}

static float easeInOut(float e) { return e < 0.5f ? 4 * e * e * e : 1 - powf(-2 * e + 2, 3) / 2; }
static float easeIn(float e) { return e * e * e; }
static float easeOut(float e) { return 1 - powf(1 - e, 3); }
static float easeSpring(float e) { return 1 - expf(-6 * e) * cosf(3 * PI_F * e); }

typedef float (*ease_fn)(float);
typedef struct { float t, v; ease_fn ease; } kf_t;

// Keyframes, each eased in from the one before
static float keys(float e, const kf_t *k, int n)
{
	if (e <= k[0].t) return k[0].v;
	for (int i = 1; i < n; i++) {
		if (e <= k[i].t) {
			ease_fn f = k[i].ease ? k[i].ease : easeInOut;
			return mixf(k[i - 1].v, k[i].v, f((e - k[i - 1].t) / (k[i].t - k[i - 1].t)));
		}
	}
	return k[n - 1].v;
}

typedef struct { float x, y, z, yaw, pitch, roll, s, lid, spread, fade; } pose_t;

enum { PH_REST, PH_OPENING, PH_OPEN, PH_CLOSING };

#define SPIN_RATE (18.0f * PI_F / 180.0f)
#define TRANSITION 1.15f
#define CYCLE 8.8f
// A plays the game: the case opens, the disc spins up, the camera runs in
// to it and the picture goes black. All timed from when the lid is open.
#define LAUNCH_SPIN_FROM -1.0f		// the disc starts to wind up
#define LAUNCH_SPIN_ACCEL 26.0f		// rad/s^2
#define LAUNCH_SPIN_MAX 34.0f		// rad/s
#define LAUNCH_ZOOM_FROM -0.3f
#define LAUNCH_ZOOM_LEN 1.6f
#define LAUNCH_FADE_FROM 0.6f
#define LAUNCH_FADE_LEN 0.7f
#define LAUNCH_DONE 1.4f
#define LAUNCH_NEAR 70.0f			// how close the camera ends to the disc
// Coming in, the scene sits under black: lifted as soon as the case to show
// has its art, but never longer than a slow device can justify, then faded.
#define INTRO_MIN 0.15f
#define INTRO_MAX 1.0f
#define INTRO_FADE 0.45f

static struct {
	float t;
	int count, cur;
	bool tr;
	int trFrom, trTo, trDir, trPhase;
	float trStart, trPhaseAt;
	int queue, queueDir, swaps;
	float clock;
	int phase;
	float phaseAt;
	bool autoOpen, closeAfter;
	float spin, spinRate;
	bool spinOn, paused;
	// the C-stick standing in for the original's drag
	bool dragActive;
	float dragTX, dragTY, dragX, dragY, dragVX, dragVY;
	// dock
	float railPos;
	int dockActive, dockPrev;
	float dockChanged;
	float toolOn[2];
	bool started;
	// playing the game
	bool launching;
	float launchAt;		// when the lid is (or was) open
	float zoom, fade;
	// coming in under black (1 covered, 0 shown) and when the cover went up
	float intro, introAt;
} jc = { .queue = -1, .spinOn = true, .spinRate = SPIN_RATE, .paused = true, .dockActive = -1, .dockPrev = -1 };

static pose_t restPose(float t, float seed)
{
	pose_t a = {0, 0, 0, 0, 0, 0, 1, 0, 0, 1};
	a.yaw = -0.2f + (0.1f * sinf(0.9f * t + seed) + 0.03f * sinf(2.1f * t + 2 * seed));
	a.pitch = -0.12f + 0.045f * sinf(0.7f * t + 1.7f * seed);
	a.roll = 0.03f + 0.018f * sinf(0.5f * t + 0.7f * seed);
	a.y = 1.8f * sinf(1.1f * t + seed);
	return a;
}

static pose_t casePose(int index, float t, int phase, float phaseAt)
{
	pose_t a = restPose(t, 1.7f * index + 0.4f);
	float r = t - phaseAt;
	if (phase == PH_OPENING) {
		float e = fminf(r, 2.3f);
		a.yaw = mixf(a.yaw, 0, sat(e / 1.0f)) + curveAt(&openYaw, e);
		a.lid = curveAt(&openLid, e);
		a.spread = curveAt(&openSpread, e);
		a.pitch = mixf(a.pitch, curveAt(&openPitch, e), sat(e / 1.45f));
		a.roll *= 1 - sat(e / 1.45f);
	}
	else if (phase == PH_OPEN) {
		a.yaw = 0.06f * sinf(0.6f * t);
		a.pitch = -0.1f + 0.02f * sinf(0.8f * t);
		a.roll = 0;
		a.lid = 3.05f + 0.025f * sinf(1.3f * t);
		a.spread = 1;
	}
	else if (phase == PH_CLOSING) {
		float e = fminf(r, 1.95f), k = easeInOut(sat((e - 0.9f) / (1.95f - 0.9f)));
		a.yaw = mixf(curveAt(&closeYaw, e), a.yaw, k);
		a.pitch = mixf(curveAt(&closePitch, e), a.pitch, k);
		a.roll = mixf(curveAt(&closeRoll, e), a.roll, k);
		a.lid = curveAt(&closeLid, e);
		a.spread = curveAt(&closeSpread, e);
	}
	return a;
}

static void openCase(bool autoOpen)
{
	if (jc.phase == PH_OPENING || jc.phase == PH_OPEN || jc.phase == PH_CLOSING) return;
	jc.phase = PH_OPENING;
	jc.phaseAt = jc.t;
	jc.autoOpen = autoOpen;
}

static void closeCase(void)
{
	if (jc.phase == PH_OPENING) {
		jc.autoOpen = true;
		jc.closeAfter = true;
		return;
	}
	if (jc.phase == PH_OPEN) {
		jc.phase = PH_CLOSING;
		jc.phaseAt = jc.t;
	}
}

// dir: +1 brings the case in from the right, -1 from the left, 0 by which way the list goes
static void goTo(int e, int dir)
{
	if (!jc.count) return;
	if (jc.tr) { jc.queue = e; jc.queueDir = dir; return; }
	if (e == jc.cur) { jc.queue = -1; return; }
	jc.tr = true;
	jc.trFrom = jc.cur;
	jc.trTo = e;
	jc.trStart = jc.t;
	// the original takes them up and down by turns; here they go left to right
	jc.trDir = dir ? dir : e > jc.cur ? 1 : -1;
	jc.trPhase = jc.phase;
	jc.trPhaseAt = jc.phaseAt;
	jc.phase = PH_REST;
	jc.phaseAt = jc.t;
	jc.autoOpen = false;
	jc.closeAfter = false;
	jc.swaps++;
}

static void step(int d)
{
	int n = jc.count;
	if (n) goTo(((jc.tr ? jc.trTo : jc.cur) + d % n + n) % n, d > 0 ? 1 : -1);
}

static void update(float dt)
{
	jc.t += dt;
	float t = jc.t;
	if (jc.launching && t >= jc.launchAt + LAUNCH_SPIN_FROM) {
		jc.spinRate = fminf(fmaxf(jc.spinRate, 0.0f) + LAUNCH_SPIN_ACCEL * dt, LAUNCH_SPIN_MAX);
	}
	else {
		float target = jc.spinOn ? SPIN_RATE : 0.0f;
		jc.spinRate += (target - jc.spinRate) * (1 - expf(-0.9f * dt * 3));
	}
	jc.spin = fmodf(jc.spin + jc.spinRate * dt, 2 * PI_F);
	if (jc.launching) {
		float z = sat((t - jc.launchAt - LAUNCH_ZOOM_FROM) / LAUNCH_ZOOM_LEN);
		jc.zoom = z * z * (2 - z);
		jc.fade = smoothstep(0, 1, (t - jc.launchAt - LAUNCH_FADE_FROM) / LAUNCH_FADE_LEN);
	}
	else {
		// called off: back out of it
		float k = expf(-6.0f * dt);
		jc.zoom *= k;
		jc.fade *= k;
	}

	if (jc.dragActive) {
		// follow the stick, and keep its speed for the flick when it's let go
		float k = 1 - expf(-12.0f * dt);
		float nx = jc.dragX + (jc.dragTX - jc.dragX) * k, ny = jc.dragY + (jc.dragTY - jc.dragY) * k;
		if (dt > 0) { jc.dragVX = (nx - jc.dragX) / dt; jc.dragVY = (ny - jc.dragY) / dt; }
		jc.dragX = nx;
		jc.dragY = ny;
	}
	else {
		jc.dragVX += (-16 * jc.dragX - 5.5f * jc.dragVX) * dt;
		jc.dragVY += (-16 * jc.dragY - 5.5f * jc.dragVY) * dt;
		jc.dragX += jc.dragVX * dt;
		jc.dragY += jc.dragVY * dt;
	}

	if (jc.tr && t - jc.trStart >= TRANSITION) {
		jc.cur = jc.trTo;
		jc.tr = false;
		jc.clock = 0;
		int q = jc.queue;
		jc.queue = -1;
		if (q >= 0 && q != jc.cur) goTo(q, jc.queueDir);
	}
	float c = t - jc.phaseAt;
	if (jc.phase == PH_OPENING && c >= 2.3f) {
		jc.phase = PH_OPEN;
		jc.phaseAt = t;
	}
	else if (jc.phase == PH_CLOSING && c >= 1.95f) {
		jc.phase = PH_REST;
		jc.phaseAt = t;
		jc.clock = fmaxf(jc.clock, 7.1f);
	}
	bool held = jc.dragActive || (jc.phase == PH_OPEN && !jc.autoOpen);
	bool idle = !jc.paused && !held && !jc.tr && !jc.launching;
	if (jc.phase == PH_OPEN && (jc.closeAfter || (jc.autoOpen && idle && t - jc.phaseAt >= 0.25f))) {
		jc.closeAfter = false;
		closeCase();
	}
	if (idle) {
		jc.clock += dt;
		// the original opens each case by itself for a moment; here only A opens one
		if (jc.phase == PH_REST && jc.clock >= CYCLE) step(1);
	}
}

/* Main thread controls */

static void jewelPrepare(void);

void JewelCase_Enter(int count, int selected)
{
	jewelPrepare();
	artLock();
	if (!jc.started || jc.count != count) {
		jc.started = true;
		jc.count = count;
		jc.cur = selected >= 0 ? (selected < count ? selected : 0) : 0;
		jc.tr = false;
		jc.queue = -1;
		jc.phase = PH_REST;
		jc.phaseAt = jc.t;
		jc.clock = 0;
		jc.railPos = (selected >= 0 && selected < count) ? selected : 0;
		jc.dockActive = jc.dockPrev = -1;
		jc.intro = 1;
		jc.introAt = jc.t;
	}
	if (jc.launching) {
		// back from a game that didn't start: come back in from black too
		jc.launching = false;
		jc.zoom = jc.fade = 0;
		jc.intro = 1;
		jc.introAt = jc.t;
		jc.phase = PH_REST;
		jc.phaseAt = jc.t;
		jc.autoOpen = jc.closeAfter = false;
	}
	else if (selected != (jc.tr ? jc.trTo : jc.cur)) {
		goTo(selected, 0);
	}
	artUnlock();
}

void JewelCase_Step(int delta) { artLock(); step(delta); artUnlock(); }
void JewelCase_GoTo(int index) { artLock(); if (index >= 0 && index < jc.count) goTo(index, 0); artUnlock(); }

void JewelCase_SetOpen(bool open)
{
	artLock();
	if (!jc.tr) {
		if (!open) jc.launching = false;
		if (open) openCase(false);
		else closeCase();
	}
	artUnlock();
}

bool JewelCase_Launch(void)
{
	artLock();
	bool ok = !jc.tr && !jc.launching && jc.phase != PH_CLOSING && jc.count > 0;
	if (ok) {
		float t = jc.t;
		if (jc.phase == PH_REST) {
			openCase(false);
			jc.launchAt = t + 2.3f;
		}
		else if (jc.phase == PH_OPENING) {
			jc.launchAt = fmaxf(jc.phaseAt + 2.3f, t - LAUNCH_SPIN_FROM);
		}
		else {
			jc.launchAt = t - LAUNCH_SPIN_FROM;
		}
		jc.autoOpen = jc.closeAfter = false;
		jc.launching = true;
	}
	artUnlock();
	return ok;
}

bool JewelCase_IsLaunching(void) { return jc.launching; }

bool JewelCase_IsCovered(void) { return jc.intro > 0.002f; }
// The picture has gone black: the game can be started
bool JewelCase_LaunchDone(void) { return jc.launching && jc.t >= jc.launchAt + LAUNCH_DONE; }

bool JewelCase_IsOpen(void) { return jc.phase == PH_OPENING || jc.phase == PH_OPEN; }
int JewelCase_Selected(void) { return jc.tr ? jc.trTo : jc.cur; }

void JewelCase_SetDrag(float x, float y, bool active)
{
	artLock();
	jc.dragActive = active;
	jc.dragTX = clampf(x, -0.45f, 0.45f);
	jc.dragTY = clampf(y, -0.9f, 0.9f);
	artUnlock();
}

void JewelCase_ToggleSpin(void) { jc.spinOn = !jc.spinOn; }
void JewelCase_TogglePause(void) { jc.paused = !jc.paused; }

// A button pressed: an open case the user opened stays open, and the idle
// clock starts over so a case isn't swapped right under them
void JewelCase_Interact(void)
{
	artLock();
	if (jc.phase == PH_REST && !jc.tr) jc.clock = 0;
	artUnlock();
}

/* ---------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------- */

typedef struct {
	int index;
	pose_t pose;
	Mtx mCase, mLid;		// model -> world
	Mtx mvCase, mvLid;		// model -> view
	float scale;
	art_t *art;
	float depthTray, depthLid;
} shown_t;

static void eulerXYZ(Mtx r, float x, float y, float z)
{
	Mtx rx, ry, rz, t;
	guMtxRotRad(rx, 'x', x);
	guMtxRotRad(ry, 'y', y);
	guMtxRotRad(rz, 'z', z);
	guMtxConcat(rx, ry, t);
	guMtxConcat(t, rz, r);
}

static float openScale;

static void placeCase(shown_t *c)
{
	pose_t *t = &c->pose;
	float a = t->s * mixf(1, openScale, t->spread);
	Mtx r, s, m;
	eulerXYZ(r, t->pitch + jc.dragX, t->yaw + jc.dragY, t->roll);
	guMtxScale(s, a, a, a);
	guMtxConcat(r, s, m);
	m[0][3] = t->x + CASE_HALF * a * t->spread * 0.92f;
	m[1][3] = t->y;
	m[2][3] = t->z;
	guMtxCopy(m, c->mCase);
	Mtx h, hi, ry, tmp;
	guMtxTrans(h, HINGE_X, 0, HINGE_Z);
	guMtxTrans(hi, -HINGE_X, 0, -HINGE_Z);
	guMtxRotRad(ry, 'y', -t->lid);
	guMtxConcat(c->mCase, h, tmp);
	guMtxConcat(tmp, ry, m);
	guMtxConcat(m, hi, c->mLid);
	guMtxConcat(view, c->mCase, c->mvCase);
	guMtxConcat(view, c->mLid, c->mvLid);
	c->scale = a;
	v3 p;
	xform(c->mvCase, V3(0, 0, -2), &p);
	c->depthTray = p.z;
	xform(c->mvLid, V3(0, 0, 4), &p);
	c->depthLid = p.z;
}

// The case stands on Swiss's own backdrop, below its title bar and above the
// dock; the original has a white page of its own and lifts the picture 44px
static bool drawPage = false;
static float viewLift = -16.0f;
static float viewFill = 0.88f;		// of the original's size, to fit between the two
static bool showDock = true;

// The plastic's light was worked out against the white page; over Swiss's dark
// backdrop it is dimmed and cooled to its lavender, and lets more through
#define GLASS_TINT_R 0.34f
#define GLASS_TINT_G 0.33f
#define GLASS_TINT_B 0.50f
#define GLASS_TAKE 0.38f		// share of the original's darkening of what's behind

static float liftNow(void) { return viewLift * (1 - jc.zoom) / 240.0f; }

static void loadProjection(void)
{
	Mtx44 proj;
	guPerspective(proj, FOV_Y, 640.0f / 480.0f, 5.0f, 4000.0f);
	float os = UI_GetOverscan();
	for (int i = 0; i < 4; i++) { proj[0][i] *= os; proj[1][i] *= os; }
	float lift = liftNow();
	proj[1][2] -= lift * os;
	GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
}

static void directVtx(bool tex)
{
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	if (tex) GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(VTXFMT_DIR, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(VTXFMT_DIR, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(VTXFMT_DIR, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
}

static void colorPipeline(void)
{
	directVtx(false);
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(0);
	GX_SetNumIndStages(0);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetTevDirect(GX_TEVSTAGE0);
}

static inline void emitColor(float x, float y, float z, u8 r, u8 g, u8 b, u8 a)
{
	GX_Position3f32(x, y, z);
	GX_Color4u8(r, g, b, a);
}

/* The white page and the case's faint shadows on it -------------------------- */

// The outlines' directions, worked out once
static float cornerDir[28][2], circleDir[48][2];

static void initOutlines(void)
{
	for (int k = 0; k < 4; k++) {
		for (int j = 0; j <= 6; j++) {
			float a = k * (PI_F / 2) + j / 6.0f * (PI_F / 2);
			cornerDir[k * 7 + j][0] = cosf(a);
			cornerDir[k * 7 + j][1] = sinf(a);
		}
	}
	for (int i = 0; i < 48; i++) {
		circleDir[i][0] = cosf(i / 48.0f * 2 * PI_F);
		circleDir[i][1] = sinf(i / 48.0f * 2 * PI_F);
	}
}

// Points round a rounded square of half size e + o, corner radius r + o
static int roundSquareOutline(float e, float r, float o, float out[][2])
{
	float rr = fmaxf(r + o, 0.0f), c = e + o - rr;
	const float cs[4][2] = {{c, c}, {-c, c}, {-c, -c}, {c, -c}};
	for (int n = 0; n < 28; n++) {
		out[n][0] = cs[n / 7][0] + rr * cornerDir[n][0];
		out[n][1] = cs[n / 7][1] + rr * cornerDir[n][1];
	}
	return 28;
}

static int circleOutline(float r, float o, float out[][2])
{
	for (int i = 0; i < 48; i++) {
		out[i][0] = (r + o) * circleDir[i][0];
		out[i][1] = (r + o) * circleDir[i][1];
	}
	return 48;
}

// Where the points of a part's plane fall on the page, along the light: for a
// plane that is a flat map, worked out once per outline
typedef struct { float ax, bx, cx, ay, by, cy; } pagemap_t;

static void pageMap(Mtx m, float z, pagemap_t *pm)
{
	// LIGHT_DIR's x and y over its z
	const float lx = LIGHT_DIR.x / LIGHT_DIR.z, ly = LIGHT_DIR.y / LIGHT_DIR.z;
	float wz = m[2][2] * z + m[2][3] - PAGE_Z;
	pm->ax = m[0][0] - lx * m[2][0];
	pm->bx = m[0][1] - lx * m[2][1];
	pm->cx = m[0][2] * z + m[0][3] - lx * wz;
	pm->ay = m[1][0] - ly * m[2][0];
	pm->by = m[1][1] - ly * m[2][1];
	pm->cy = m[1][2] * z + m[1][3] - ly * wz;
}

static inline void onPage(const pagemap_t *pm, float x, float y, float *px, float *py)
{
	*px = pm->ax * x + pm->bx * y + pm->cx;
	*py = pm->ay * x + pm->by * y + pm->cy;
}

// The shadow of a part's outline: rings at even steps of offset from it, each
// darkened by the original's shadow formula at that distance. Inside the first
// ring a filled outline keeps that ring's shade.
typedef float (*shadow_fn)(float o, float soft);
#define SHADOW_STEPS 14

static void shadowProfile(Mtx m, float z, bool square, float size, float radius, float o0, float o1, shadow_fn k, float soft, float fade)
{
	float a[64][2], pa[64][2], pb[64][2];
	int cnt = 0;
	pagemap_t pm;
	pageMap(m, z, &pm);
	// each ring's outline is put on the page once, and kept for the next band
	float (*prev)[2] = pa, (*next)[2] = pb;
	u8 cprev = 0;
	for (int i = 0; i <= SHADOW_STEPS; i++) {
		float o = o0 + (o1 - o0) * i / SHADOW_STEPS;
		cnt = square ? roundSquareOutline(size, radius, o, a) : circleOutline(size, o, a);
		for (int j = 0; j < cnt; j++) onPage(&pm, a[j][0], a[j][1], &next[j][0], &next[j][1]);
		u8 c = toSrgb8(1.0f - k(o, soft) * fade);
		if (i > 0 && !(cprev == 255 && c == 255)) {
			GX_Begin(GX_TRIANGLESTRIP, VTXFMT_DIR, (cnt + 1) * 2);
			for (int j = 0; j <= cnt; j++) {
				emitColor(prev[j % cnt][0], prev[j % cnt][1], PAGE_Z, cprev, cprev, cprev, 255);
				emitColor(next[j % cnt][0], next[j % cnt][1], PAGE_Z, c, c, c, 255);
			}
			GX_End();
		}
		float (*t)[2] = prev; prev = next; next = t;
		cprev = c;
	}
	if (!square) return;
	u8 c = toSrgb8(1.0f - k(o0, soft) * fade);
	cnt = roundSquareOutline(size, radius, o0, a);
	float cx, cy;
	onPage(&pm, 0, 0, &cx, &cy);
	GX_Begin(GX_TRIANGLEFAN, VTXFMT_DIR, cnt + 2);
	emitColor(cx, cy, PAGE_Z, c, c, c, 255);
	for (int j = 0; j <= cnt; j++) {
		float px, py;
		onPage(&pm, a[j % cnt][0], a[j % cnt][1], &px, &py);
		emitColor(px, py, PAGE_Z, c, c, c, 255);
	}
	GX_End();
}

// the tray: a pale square, deeper along the rails (d: how far outside its edge)
static float trayShade(float d, float soft)
{
	float cover = 1.0f - smoothstep(-soft, soft, d);
	float rails = cover * smoothstep(-14.0f - soft, -9.0f + soft, d);
	return 0.02f * cover + 0.08f * rails;
}

// the lid: fainter, a little deeper round its rim
static float lidShade(float d, float soft)
{
	float cover = 1.0f - smoothstep(-soft, soft, d);
	float rim = cover * smoothstep(-4.0f - soft, -1.0f + soft, d);
	return 0.03f * cover + 0.05f * rim;
}

// the disc (d: how far outside its hole)
static float discShade(float d, float soft)
{
	float r = d + 7.5f;
	return 0.2f * (1.0f - smoothstep(60.0f - soft, 60.0f + soft, r)) * smoothstep(7.5f - soft, 7.5f + soft, r);
}

static float shadowSoft(Mtx m, float z)
{
	v3 w;
	xform(m, V3(0, 0, z), &w);
	float dist = (w.z - PAGE_Z) / vnorm(LIGHT_DIR).z;
	return 0.9f + dist * 0.075f;
}

static void drawShadows(shown_t *cases, int n)
{
	colorPipeline();
	GX_LoadPosMtxImm(view, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	GX_SetZMode(GX_DISABLE, GX_ALWAYS, GX_FALSE);
	// multiply the page by the shade
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_ZERO, GX_BL_SRCCLR, GX_LO_CLEAR);
	for (int i = 0; i < n; i++) {
		shown_t *c = &cases[i];
		float f = c->pose.fade;
		if (f <= 0.0f) continue;
		// the tray, the disc, and the lid wherever it has swung to
		float s = shadowSoft(c->mCase, -2.0f);
		shadowProfile(c->mCase, -2.0f, true, CASE_HALF, 8.0f, -14.0f - s, s, trayShade, s, f);
		s = shadowSoft(c->mCase, -1.8f);
		shadowProfile(c->mCase, -1.8f, false, 7.5f, 0, -s, 52.5f + s, discShade, s, f);
		s = shadowSoft(c->mLid, 4.0f);
		shadowProfile(c->mLid, 4.0f, true, CASE_HALF, 8.0f, -4.0f - s, s, lidShade, s, f);
	}
}

/* The disc --------------------------------------------------------------------- */

#define DISC_SEGS 48
#define DISC_LIGHT_STEP 2		// the silver is lit on every other point round, DISC_SEGS a multiple
static const float discRings[] = {15.0f, 16.0f, 16.4f, 16.6f, 16.8f, 17.2f, 19.0f, 19.9f, 20.3f, 22.0f, 30.0f, 40.0f, 50.0f, 58.0f, 58.6f, 59.1f, 59.5f, 60.0f};
#define DISC_RINGS (sizeof(discRings) / sizeof(discRings[0]))
#define ART_IN 20.1f
#define PRINT_OUT 59.1f
static float discCos[DISC_SEGS + 1], discSin[DISC_SEGS + 1];

// The bare disc: aluminium under lacquer, a bright satin mirror with a little of the room in it
static v3 silver(v3 R, v3 N, float r)
{
	v3 st = studio(R);
	v3 c = vadd(vmul(vmin(st, 1.6f), 0.5f), vmul(V3(0.42f, 0.44f, 0.47f), 0.6f + 0.4f * fmaxf(vdot(N, KEY), 0.0f)));
	// the stacking ring in the mirror band
	c = vmul(c, 1.0f - 0.35f * (1.0f - smoothstep(0.15f, 0.4f, fabsf(r - 16.6f))));
	// and its last millimetre is clear plastic over the silver's edge
	float rim = smoothstep(58.0f, 58.9f, r);
	if (rim <= 0) return c;
	return vmix(c, vadd(vmul(st, 0.35f), V3(0.22f, 0.23f, 0.25f)), rim * 0.85f);
}

// The rainbow a light draws through the middle of the pressed track, as a strip of quads
static void drawSpokes(Mtx mv, float zFace, float nzLocal, float fade)
{
	const v3 *L[4] = {&KEY, &STRIP, &TOP, &BOUNCE};
	const float gain[4] = {1.6f, 1.4f, 0.9f, 0.35f}, spread[4] = {0.05f, 0.045f, 0.06f, 0.16f};
	v3 centre, N, ex, ey;
	xform(mv, V3(0, 0, zFace), &centre);
	xformDir(mv, V3(0, 0, nzLocal), &N);
	xformDir(mv, V3(1, 0, 0), &ex);
	xformDir(mv, V3(0, 1, 0), &ey);
	float sc = vlen(ex);
	N = vnorm(N);
	ex = vnorm(ex);
	ey = vnorm(ey);
	v3 V0 = vnorm(vmul(centre, -1));
	if (vdot(N, V0) <= 0.0f) return;

	GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
	for (int l = 0; l < 4; l++) {
		v3 H = vadd(*L[l], V0);
		float hx = vdot(H, ex), hy = vdot(H, ey);
		float hl = sqrtf(hx * hx + hy * hy);
		float theta = atan2f(hy, hx);
		float dth = fminf(spread[l] * 2.2f / fmaxf(hl, 1e-3f), 0.5f);
		for (int side = 0; side < 2; side++) {
			float th = theta + side * PI_F;
			const int RS = 12, CS = 4;
			// the directions across the band, and the silver under its two edges on
			// each ring, to blend between (it is narrow, the silver smooth across it)
			float ca[CS + 1], sa[CS + 1];
			for (int ci = 0; ci <= CS; ci++) {
				float a = th + dth * ((ci / (float)CS) * 2.0f - 1.0f);
				ca[ci] = cosf(a);
				sa[ci] = sinf(a);
			}
			v3 under[RS + 1][2];
			float data[RS + 1];
			for (int ri = 0; ri <= RS; ri++) {
				float r = 21.5f + (58.8f - 21.5f) * ri / RS;
				for (int e = 0; e < 2; e++) {
					v3 P;
					xform(mv, V3(r * ca[e * CS], r * sa[e * CS], zFace), &P);
					v3 V = vnorm(vmul(P, -1));
					under[ri][e] = silver(vreflect(vmul(V, -1), N), N, r);
				}
				data[ri] = smoothstep(21.5f, 23.0f, r) * (1 - smoothstep(58.2f, 58.8f, r));
			}
			for (int ci = 0; ci < CS; ci++) {
				GX_Begin(GX_TRIANGLESTRIP, VTXFMT_DIR, (RS + 1) * 2);
				for (int ri = 0; ri <= RS; ri++) {
					float r = 21.5f + (58.8f - 21.5f) * ri / RS;
					for (int k = 0; k < 2; k++) {
						float cs = ca[ci + k], sn = sa[ci + k];
						float x = r * cs, y = r * sn;
						v3 P;
						xform(mv, V3(x, y, zFace), &P);
						v3 V = vnorm(vmul(P, -1));
						v3 h = vadd(*L[l], V);
						v3 rad = vadd(vmul(ex, cs), vmul(ey, sn));
						v3 tan = vadd(vmul(ex, -sn), vmul(ey, cs));
						float along = vdot(h, tan), u = fabsf(vdot(h, rad));
						float w = expf(-along * along / (spread[l] * spread[l]));
						v3 c = vadd(vadd(spectral(1600 * u), vmul(spectral(800 * u), 0.75f)), vmul(spectral(533.3f * u), 0.45f));
						c = vmul(c, w * gain[l] * 1.3f * data[ri] * fade);
						// added on top of the mirror, in display light
						v3 base = vmix(under[ri][0], under[ri][1], (float)(ci + k) / CS);
						u8 cr = toSrgb8(base.x + c.x) - toSrgb8(base.x);
						u8 cg = toSrgb8(base.y + c.y) - toSrgb8(base.y);
						u8 cb = toSrgb8(base.z + c.z) - toSrgb8(base.z);
						emitColor(x, y, zFace + nzLocal * 0.02f / sc, cr, cg, cb, 255);
					}
				}
				GX_End();
			}
		}
	}
}

// The art turns with the disc: rotate the texture rather than the mesh
static void loadArtMtx(float spin)
{
	Mtx tm;
	float cs = cosf(spin), sn = sinf(spin);
	guMtxIdentity(tm);
	tm[0][0] = cs / 120.0f; tm[0][1] = -sn / 120.0f; tm[0][3] = 0.5f;
	tm[1][0] = -sn / 120.0f; tm[1][1] = -cs / 120.0f; tm[1][3] = 0.5f;
	GX_LoadTexMtxImm(tm, GX_TEXMTX2, GX_MTX2x4);
}

static void drawDisc(shown_t *c)
{
	Mtx mv;
	guMtxCopy(c->mvCase, mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	float fade = c->pose.fade;
	u8 fa = (u8)(sat(fade) * 255);
	GX_SetZMode(GX_ENABLE, GX_LEQUAL, GX_TRUE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

	v3 N, NB;
	xformDir(mv, V3(0, 0, 1), &N);
	N = vnorm(N);
	NB = vmul(N, -1);
	art_t *a = c->art && !c->art->missing ? c->art : NULL;
	const float zf = -1.2f, zb = -2.4f;
	// only the face turned to the camera is drawn: the disc is thinner than the depth buffer can tell apart
	v3 V0 = vnorm(V3(-mv[0][3], -mv[1][3], -mv[2][3]));
	bool back = vdot(N, V0) < 0;

	// the printed side, or the bare silver when there's no art
	if (a && !back) {
		directVtx(true);
		GX_SetNumChans(1);
		GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
		GX_SetNumTexGens(1);
		loadArtMtx(jc.spin);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX2);
		GX_SetNumIndStages(0);
		GX_SetNumTevStages(2);
		GX_InvalidateTexAll();
		GX_LoadTexObj(&a->texObj, GX_TEXMAP0);
		// stage 0: the ink, lit by the room; its alpha is the art's where it's printed
		float lit = powf(lights(N), 1.0f / 2.2f);
		u8 kl = (u8)(clampf(lit * 0.5f, 0, 1) * 255);
		GX_SetTevKColor(GX_KCOLOR0, (GXColor) {kl, kl, kl, fa});
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
		GX_SetTevDirect(GX_TEVSTAGE0);
		GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
		GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
		GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
		GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		// stage 1: over the silver
		GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevDirect(GX_TEVSTAGE1);
		GX_SetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_K0_A);
		GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_RASC, GX_CC_CPREV, GX_CC_APREV, GX_CC_ZERO);
		GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
		GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	}
	else {
		colorPipeline();
	}

	// ring by ring; under the print the silver can't be seen, so it takes one colour there
	{
		v3 Nf = back ? NB : N;
		float z = back ? zb : zf;
		bool printing = a && !back;
		v3 flat = silver(vreflect(vmul(V0, -1), Nf), Nf, 40.0f);
		u8 flatC[3] = {toSrgb8(flat.x), toSrgb8(flat.y), toSrgb8(flat.z)};
		// each ring's colours once: the silver on every third point, the rest between
		static u8 ringCol[DISC_RINGS][DISC_SEGS + 1][4];
		float ringPrinted[DISC_RINGS];
		for (u32 ri = 0; ri < DISC_RINGS; ri++) {
			float r = discRings[ri];
			float printed = printing ? smoothstep(ART_IN - 0.2f, ART_IN + 0.2f, r) * (1 - smoothstep(PRINT_OUT, PRINT_OUT + 0.4f, r)) : 0.0f;
			ringPrinted[ri] = printed;
			u8 alpha = printing ? (u8)(printed * 255) : fa;
			u8 (*row)[4] = ringCol[ri];
			if (printing && r >= ART_IN + 1.5f && r <= 57.5f) {
				for (int si = 0; si <= DISC_SEGS; si++) {
					row[si][0] = flatC[0]; row[si][1] = flatC[1]; row[si][2] = flatC[2]; row[si][3] = alpha;
				}
				continue;
			}
			for (int si = 0; si <= DISC_SEGS; si += DISC_LIGHT_STEP) {
				v3 P;
				xform(mv, V3(r * discCos[si], r * discSin[si], z), &P);
				v3 V = vnorm(vmul(P, -1));
				v3 col = silver(vreflect(vmul(V, -1), Nf), Nf, r);
				row[si][0] = toSrgb8(col.x); row[si][1] = toSrgb8(col.y); row[si][2] = toSrgb8(col.z); row[si][3] = alpha;
			}
			for (int si = 0; si < DISC_SEGS; si += DISC_LIGHT_STEP) {
				for (int j = 1; j < DISC_LIGHT_STEP; j++) {
					for (int c = 0; c < 4; c++) {
						row[si + j][c] = (u8)((row[si][c] * (DISC_LIGHT_STEP - j) + row[si + DISC_LIGHT_STEP][c] * j) / DISC_LIGHT_STEP);
					}
				}
			}
		}
		for (u32 ri = 0; ri + 1 < DISC_RINGS; ri++) {
			GX_Begin(GX_TRIANGLESTRIP, VTXFMT_DIR, (DISC_SEGS + 1) * 2);
			for (int si = 0; si <= DISC_SEGS; si++) {
				for (int k = 0; k < 2; k++) {
					float r = discRings[ri + k];
					float x = r * discCos[si], y = r * discSin[si];
					const u8 *c = ringCol[ri + k][si];
					GX_Position3f32(x, y, z);
					GX_Color4u8(c[0], c[1], c[2], c[3]);
					if (printing) GX_TexCoord2f32(x, y);
				}
			}
			GX_End();
		}

		// spun up to play, the print smears round: the angles it went through
		// this frame laid over it, each pass weighted to an even average
		// (none once the picture is mostly black, where they'd only cost fill)
		float sweep = jc.spinRate / 60.0f;
		int passes = printing && jc.fade < 0.7f ? (int)fminf(ceilf(sweep / 0.09f), 4) : 0;
		if (passes >= 2) {
			GX_SetNumTevStages(1);
			GX_SetZMode(GX_ENABLE, GX_LEQUAL, GX_FALSE);
			for (int p = 1; p < passes; p++) {
				loadArtMtx(jc.spin - sweep * p / passes);
				float w = sat(fade) / (p + 1);
				for (u32 ri = 0; ri + 1 < DISC_RINGS; ri++) {
					if (ringPrinted[ri] <= 0 && ringPrinted[ri + 1] <= 0) continue;
					u8 ra[2] = {(u8)(ringPrinted[ri] * w * 255), (u8)(ringPrinted[ri + 1] * w * 255)};
					GX_Begin(GX_TRIANGLESTRIP, VTXFMT_DIR, (DISC_SEGS + 1) * 2);
					for (int si = 0; si <= DISC_SEGS; si++) {
						for (int k = 0; k < 2; k++) {
							float r = discRings[ri + k];
							float x = r * discCos[si], y = r * discSin[si];
							GX_Position3f32(x, y, zf);
							GX_Color4u8(255, 255, 255, ra[k]);
							GX_TexCoord2f32(x, y);
						}
					}
					GX_End();
				}
			}
			GX_SetZMode(GX_ENABLE, GX_LEQUAL, GX_TRUE);
		}
	}

	// the edge: a CD's clear plastic seen end on
	colorPipeline();
	GX_Begin(GX_TRIANGLESTRIP, VTXFMT_DIR, (DISC_SEGS + 1) * 2);
	for (int si = 0; si <= DISC_SEGS; si++) {
		v3 n, P;
		xformDir(mv, V3(discCos[si], discSin[si], 0), &n);
		n = vnorm(n);
		for (int k = 0; k < 2; k++) {
			float z = k ? zb : zf;
			xform(mv, V3(60 * discCos[si], 60 * discSin[si], z), &P);
			v3 V = vnorm(vmul(P, -1));
			v3 col = vadd(V3(0.3f, 0.31f, 0.33f), vmul(studio(vreflect(vmul(V, -1), n)), 0.25f));
			emitColor(60 * discCos[si], 60 * discSin[si], z, toSrgb8(col.x), toSrgb8(col.y), toSrgb8(col.z), fa);
		}
	}
	GX_End();

	// the rainbow on the data side (and the front, when nothing is printed on it)
	GX_SetZMode(GX_ENABLE, GX_LEQUAL, GX_FALSE);
	if (back) drawSpokes(mv, zb, -1.0f, fade);
	else if (!a) drawSpokes(mv, zf, 1.0f, fade);
}

/* The plastic ------------------------------------------------------------------ */

static void drawRods(rodmesh_t *m, Mtx mv, float scale, float fade)
{
	rodSetupVtx();
	GX_SetArray(GX_VA_POS, m->pos, 12);
	GX_SetArray(GX_VA_NRM, m->nrm, 12);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	// the normal in view space picks the texel: u = 0.5 + 0.5 nx, v = 0.5 - 0.5 ny
	Mtx tm;
	guMtxIdentity(tm);
	for (int i = 0; i < 3; i++) {
		tm[0][i] = 0.5f * mv[0][i] / scale;
		tm[1][i] = -0.5f * mv[1][i] / scale;
	}
	tm[0][3] = 0.5f;
	tm[1][3] = 0.5f;
	GX_LoadTexMtxImm(tm, GX_TEXMTX1, GX_MTX2x4);
	GX_SetNumChans(0);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_NRM, GX_TEXMTX1);
	GX_SetNumIndStages(0);
	GX_SetNumTevStages(1);
	GX_InvalidateTexAll();
	GX_LoadTexObj(&matcapTex, GX_TEXMAP0);
	float f = sat(fade);
	if (drawPage) GX_SetTevColor(GX_TEVREG0, (GXColor) {(u8)(f * 255), (u8)(f * 255), (u8)(f * 255), (u8)(f * 255)});
	else GX_SetTevColor(GX_TEVREG0, (GXColor) {(u8)(f * GLASS_TINT_R * 255), (u8)(f * GLASS_TINT_G * 255),
		(u8)(f * GLASS_TINT_B * 255), (u8)(f * GLASS_TAKE * 255)});
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
	// colour: the studio's share; alpha: how much of what's behind comes through
	// (1 - a0 + a0 * texa: a0 below the fade lets more through)
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C0, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_KONST, GX_CA_TEXA, GX_CA_A0, GX_CA_ZERO);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_SRCALPHA, GX_LO_CLEAR);
	GX_SetZMode(GX_ENABLE, GX_LEQUAL, GX_TRUE);
	GX_CallDispList(m->dl, m->dlSize);
}

// A flat sheet: face on it takes almost nothing away, at a slant it turns to a mirror
static void sheetShade(v3 P, v3 N, float thick, float fade, u8 *rgba)
{
	v3 V = vnorm(vmul(P, -1));
	float d = vdot(N, V);
	float cosv = fabsf(d);
	if (d < 0) N = vmul(N, -1);
	float F = fresnel(cosv, IOR);
	// face on: fresnel(1, IOR) and the same through both surfaces
	const float F0 = ((IOR - 1) / (IOR + 1)) * ((IOR - 1) / (IOR + 1));
	const float Fall0 = F0 + (1 - F0) * (1 - F0) * F0;
	float Fall = F + (1 - F) * (1 - F) * F;
	float cosT = sqrtf(fmaxf(1 - (1 - cosv * cosv) / (IOR * IOR), 0));
	float absorbed = 1 - expf(-0.00327f * thick / fmaxf(cosT, 0.2f));
	float takes = sat(fmaxf(Fall - Fall0, 0) + Fall0 * 0.05f + absorbed * 0.5f);
	v3 R = vreflect(vmul(V, -1), N);
	v3 g = vmin(vmul(glare(R), Fall * 0.25f), 0.1f);
	v3 refl = vadd(g, vmul(vmin(roomT(R), 1.2f), fmaxf(Fall - Fall0, 0)));
	refl = vadd(refl, vmul(V3(0.86f, 0.9f, 0.93f), (1 - cosv) * 0.03f));
	refl = vmul(refl, fade);
	takes *= fade;
	if (!drawPage) {
		// over the dark backdrop: the light it adds, plainly, and less taken away
		rgba[0] = (u8)(sat(refl.x * GLASS_TINT_R * 2) * 255);
		rgba[1] = (u8)(sat(refl.y * GLASS_TINT_G * 2) * 255);
		rgba[2] = (u8)(sat(refl.z * GLASS_TINT_B * 2) * 255);
		rgba[3] = (u8)(takes * GLASS_TAKE * 255);
		return;
	}
	// blended as src + dst * (1 - takes): exact over the white page
	float keep = 1 - takes;
	rgba[0] = (u8)(sat(toSrgb(refl.x + keep) - keep) * 255);
	rgba[1] = (u8)(sat(toSrgb(refl.y + keep) - keep) * 255);
	rgba[2] = (u8)(sat(toSrgb(refl.z + keep) - keep) * 255);
	rgba[3] = (u8)(takes * 255);
}

static void drawSheet(const sheet_t *s, Mtx mv, float scale, float fade)
{
	v3 n;
	xformDir(mv, V3(0, 0, 1), &n);
	n = vnorm(n);
	float thick = (s->z1 - s->z0) * scale;
	// both faces look the same from a point so close: shade each point once, and
	// only every other point of every other ring, the light being smooth across it
	static u8 shade[(SHEET_RINGS + 1) * 128][4];
	int segs = s->segs, pts = (SHEET_RINGS + 1) * segs;
	if (pts > (int)(sizeof(shade) / sizeof(shade[0])) || (segs & 1)) return;
	float zm = (s->z0 + s->z1) / 2;
	for (int r = 0; r <= SHEET_RINGS; r += 2) {
		for (int i = 0; i < segs; i += 2) {
			int idx = r * segs + i;
			v3 P;
			xform(mv, V3(s->xy[idx * 2], s->xy[idx * 2 + 1], zm), &P);
			sheetShade(P, n, thick, fade, shade[idx]);
		}
		for (int i = 1; i < segs; i += 2) {
			const u8 *a = shade[r * segs + i - 1], *b = shade[r * segs + (i + 1) % segs];
			u8 *d = shade[r * segs + i];
			for (int c = 0; c < 4; c++) d[c] = (a[c] + b[c] + 1) >> 1;
		}
	}
	for (int r = 1; r < SHEET_RINGS; r += 2) {
		for (int i = 0; i < segs; i++) {
			const u8 *a = shade[(r - 1) * segs + i], *b = shade[(r + 1) * segs + i];
			u8 *d = shade[r * segs + i];
			for (int c = 0; c < 4; c++) d[c] = (a[c] + b[c] + 1) >> 1;
		}
	}
	// and both faces in one: over what's behind, the second takes (1 - a) of the first's
	// light and darkens the same again
	for (int i = 0; i < pts; i++) {
		u8 *d = shade[i];
		int keep = 255 - d[3];
		for (int c = 0; c < 3; c++) { int v = d[c] + d[c] * keep / 255; d[c] = v > 255 ? 255 : v; }
		d[3] = 255 - keep * keep / 255;
	}
	for (int r = 0; r < SHEET_RINGS; r++) {
		GX_Begin(GX_TRIANGLESTRIP, VTXFMT_DIR, (segs + 1) * 2);
		for (int i = 0; i <= segs; i++) {
			for (int k = 0; k < 2; k++) {
				int idx = (r + k) * segs + i % segs;
				const float *p = &s->xy[idx * 2];
				emitColor(p[0], p[1], zm, shade[idx][0], shade[idx][1], shade[idx][2], shade[idx][3]);
			}
		}
		GX_End();
	}
}

/* The lid's sheen: the streaks a strip light leaves on a glass top. Where on the
 * face they fall follows how the face is turned: the line of sight reflected in
 * it, measured against the light, moves them across the face (with some gain, as
 * the case is a lot smaller than the light is far), so they sweep over the lid
 * as it tilts. They run corner to corner; their sharp profile is a small texture
 * looked up from a few points, along which the measure changes smoothly. */

#define SHEEN_GRID 6			// points along each side of the face
#define SHEEN_SPAN 0.6f			// how far either way (in face widths) the texture reaches
#define SHEEN_GAIN 1.3f			// face widths the streaks move per radian of reflection
#define SHEEN_REST 0.45f		// the reflection's measure at rest, where they start out
#define SHEEN_AT 0.22f			// and where on the face that puts them
#define SHEEN_TEX 128
static u8 sheenData[SHEEN_TEX * 4] ATTRIBUTE_ALIGN(32);
static GXTexObj sheenTex;
static float sheenPts[SHEEN_GRID * SHEEN_GRID][2];

static void buildSheen(void)
{
	// across the streaks: a bright one with a soft halo, and a thin one beside it (I8, 8x4 tiles)
	for (int x = 0; x < SHEEN_TEX; x++) {
		float d = ((x + 0.5f) / SHEEN_TEX * 2 - 1) * SHEEN_SPAN;
		float e = (d - 0.13f) / 0.012f;
		float k = 0.75f * expf(-(d / 0.035f) * (d / 0.035f)) + 0.25f * expf(-(d / 0.16f) * (d / 0.16f)) + 0.45f * expf(-e * e);
		for (int y = 0; y < 4; y++) sheenData[(x / 8) * 32 + y * 8 + (x % 8)] = (u8)(sat(k) * 255);
	}
	DCFlushRange(sheenData, sizeof(sheenData));
	GX_InitTexObj(&sheenTex, sheenData, SHEEN_TEX, 4, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&sheenTex, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_DISABLE, GX_DISABLE, GX_ANISO_1);
	// a grid over the lid's face, its corners pulled in to the rounded ones
	const float e = CASE_HALF - 0.5f, rc = 8.0f, c = e - rc;
	for (int j = 0; j < SHEEN_GRID; j++) {
		for (int i = 0; i < SHEEN_GRID; i++) {
			float x = -e + 2 * e * i / (SHEEN_GRID - 1), y = -e + 2 * e * j / (SHEEN_GRID - 1);
			float dx = fabsf(x) - c, dy = fabsf(y) - c;
			if (dx > 0 && dy > 0) {
				float l = sqrtf(dx * dx + dy * dy), k = rc / l;
				x = copysignf(c + dx * k, x);
				y = copysignf(c + dy * k, y);
			}
			sheenPts[j * SHEEN_GRID + i][0] = x;
			sheenPts[j * SHEEN_GRID + i][1] = y;
		}
	}
}

static void drawSheen(Mtx mv, float z, float fade)
{
	if (fade <= 0) return;
	v3 N;
	xformDir(mv, V3(0, 0, 1), &N);
	N = vnorm(N);
	// square to the light's strip, in view space: tilting the top of the case
	// towards the upper left takes the streaks that way
	static const v3 A = {-0.62f, 0.70f, 0.0f};
	float u[SHEEN_GRID * SHEEN_GRID];
	bool any = false;
	for (int i = 0; i < SHEEN_GRID * SHEEN_GRID; i++) {
		v3 P;
		xform(mv, V3(sheenPts[i][0], sheenPts[i][1], z), &P);
		v3 V = vnorm(vmul(P, -1));
		v3 n = vdot(N, V) < 0 ? vmul(N, -1) : N;
		v3 R = vreflect(vmul(V, -1), n);
		// across the face, corner to corner, in face widths
		float q = (sheenPts[i][1] - sheenPts[i][0]) / (2 * 1.41421f * CASE_HALF);
		float d = q - (SHEEN_AT + SHEEN_GAIN * (vdot(R, A) - SHEEN_REST));
		u[i] = 0.5f + d / (2 * SHEEN_SPAN);
		if (fabsf(d) < SHEEN_SPAN) any = true;
	}
	// nowhere near the face: nothing to draw
	if (!any) return;

	directVtx(true);
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetNumIndStages(0);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GX_InvalidateTexAll();
	GX_LoadTexObj(&sheenTex, GX_TEXMAP0);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	// added light, a little cooler over Swiss's backdrop
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
	GX_SetZMode(GX_ENABLE, GX_LEQUAL, GX_FALSE);
	float k = sat(fade) * (drawPage ? 0.5f : 0.42f);
	u8 cr = (u8)(k * (drawPage ? 255 : 225)), cg = (u8)(k * (drawPage ? 255 : 222)), cb = (u8)(k * 255);
	for (int j = 0; j + 1 < SHEEN_GRID; j++) {
		GX_Begin(GX_TRIANGLESTRIP, VTXFMT_DIR, SHEEN_GRID * 2);
		for (int i = 0; i < SHEEN_GRID; i++) {
			for (int r = 0; r < 2; r++) {
				int idx = (j + r) * SHEEN_GRID + i;
				GX_Position3f32(sheenPts[idx][0], sheenPts[idx][1], z);
				GX_Color4u8(cr, cg, cb, 255);
				GX_TexCoord2f32(u[idx], 0.5f);
			}
		}
		GX_End();
	}
}

static void drawSheets(const sheet_t *sheets, int n, Mtx mv, float scale, float fade)
{
	colorPipeline();
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetZMode(GX_ENABLE, GX_LEQUAL, GX_FALSE);
	for (int i = 0; i < n; i++) drawSheet(&sheets[i], mv, scale, fade);
}

/* The dock: the discs as thumbnails, and the three tools ---------------------- */

static const GXColor INK = {17, 17, 20, 255};

// The dock in Swiss's colours (the original's: a white panel, ink on it)
static GXColor dockInk(void) { return drawPage ? INK : (GXColor) {226, 222, 255, 255}; }
static GXColor dockOn(void) { return drawPage ? INK : THEME_ACCENT; }

static void uiPipeline(void)
{
	Mtx m;
	guMtxIdentity(m);
	UI_ApplyTransform(m, m);
	GX_LoadPosMtxImm(m, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	UI_LoadProjection();
	colorPipeline();
	GX_SetZMode(GX_DISABLE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
}

static void fillRoundRect(float x0, float y0, float x1, float y1, float r, GXColor c)
{
	float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
	r = fminf(r, fminf(x1 - x0, y1 - y0) / 2);
	const float cs[4][3] = {{x1 - r, y1 - r, 0}, {x0 + r, y1 - r, PI_F / 2}, {x0 + r, y0 + r, PI_F}, {x1 - r, y0 + r, 3 * PI_F / 2}};
	GX_Begin(GX_TRIANGLEFAN, VTXFMT_DIR, 4 * 9 + 2);
	emitColor(cx, cy, 0, c.r, c.g, c.b, c.a);
	for (int k = 0; k < 4; k++) {
		for (int j = 0; j <= 8; j++) {
			float a = cs[k][2] + j / 8.0f * (PI_F / 2);
			emitColor(cs[k][0] + r * cosf(a), cs[k][1] + r * sinf(a), 0, c.r, c.g, c.b, c.a);
		}
	}
	emitColor(x1, y1 - r, 0, c.r, c.g, c.b, c.a);
	GX_End();
}

// A soft-edged rounded rectangle: from alpha a inside (inset by in) to nothing at grow
static void softRoundRect(float x0, float y0, float x1, float y1, float r, float in, float grow, GXColor c)
{
	float pa[36][2], pb[36][2];
	// a blur wider than the box can't shrink it past its middle
	in = fminf(in, fminf(x1 - x0, y1 - y0) / 2 - 0.5f);
	for (int pass = 0; pass < 2; pass++) {
		float o = pass ? grow : -in, rr = fmaxf(r + o, 0.0f);
		float (*p)[2] = pass ? pb : pa;
		float ax0 = x0 - o + rr, ax1 = x1 + o - rr, ay0 = y0 - o + rr, ay1 = y1 + o - rr;
		const float cs[4][3] = {{ax1, ay1, 0}, {ax0, ay1, PI_F / 2}, {ax0, ay0, PI_F}, {ax1, ay0, 3 * PI_F / 2}};
		int n = 0;
		for (int k = 0; k < 4; k++) {
			for (int j = 0; j <= 8; j++) {
				float a = cs[k][2] + j / 8.0f * (PI_F / 2);
				p[n][0] = cs[k][0] + rr * cosf(a);
				p[n][1] = cs[k][1] + rr * sinf(a);
				n++;
			}
		}
	}
	GX_Begin(GX_TRIANGLESTRIP, VTXFMT_DIR, 37 * 2);
	for (int j = 0; j <= 36; j++) {
		emitColor(pa[j % 36][0], pa[j % 36][1], 0, c.r, c.g, c.b, c.a);
		emitColor(pb[j % 36][0], pb[j % 36][1], 0, c.r, c.g, c.b, 0);
	}
	GX_End();
	fillRoundRect(x0 + in, y0 + in, x1 - in, y1 - in, fmaxf(r - in, 0), c);
}

static void strokeArc(float cx, float cy, float r, float a0, float a1, GXColor c)
{
	int n = 24;
	GX_Begin(GX_LINESTRIP, VTXFMT_DIR, n + 1);
	for (int i = 0; i <= n; i++) {
		float a = a0 + (a1 - a0) * i / n;
		emitColor(cx + r * cosf(a), cy + r * sinf(a), 0, c.r, c.g, c.b, c.a);
	}
	GX_End();
}

static void strokePoly(const float (*p)[2], int n, bool closed, float ox, float oy, float k, GXColor c)
{
	GX_Begin(GX_LINESTRIP, VTXFMT_DIR, n + (closed ? 1 : 0));
	for (int i = 0; i < n + (closed ? 1 : 0); i++) {
		emitColor(ox + p[i % n][0] * k, oy + p[i % n][1] * k, 0, c.r, c.g, c.b, c.a);
	}
	GX_End();
}

static void fillCircle(float cx, float cy, float r, GXColor c)
{
	GX_Begin(GX_TRIANGLEFAN, VTXFMT_DIR, 34);
	emitColor(cx, cy, 0, c.r, c.g, c.b, c.a);
	for (int i = 0; i <= 32; i++) {
		float a = i / 32.0f * 2 * PI_F;
		emitColor(cx + r * cosf(a), cy + r * sinf(a), 0, c.r, c.g, c.b, c.a);
	}
	GX_End();
}

// The tools, drawn from the original's 24 unit icons
static void drawTool(int which, float cx, float cy, float size, float on)
{
	GXColor ink = dockInk(), bg = dockOn();
	if (on > 0) fillCircle(cx, cy, size / 2, (GXColor) {bg.r, bg.g, bg.b, (u8)(on * 255)});
	GXColor c = {(u8)mixf(ink.r, 255, on), (u8)mixf(ink.g, 255, on), (u8)mixf(ink.b, 255, on), 255};
	float icon = which == 2 ? 14.0f : 18.0f, k = icon / 24.0f;
	float ox = cx - 12 * k, oy = cy - 12 * k;
	GX_SetLineWidth((u8)lrintf(1.6f * k * 6.0f + 3), GX_TO_ZERO);
	if (which == 0) {
		strokeArc(cx, cy, 8.5f * k, 0, 2 * PI_F, c);
		strokeArc(cx, cy, 2.4f * k, 0, 2 * PI_F, c);
		strokeArc(cx, cy, 6.2f * k, -PI_F / 2, 0, c);
	}
	else if (which == 1) {
		const float spine[4][2] = {{11, 5.5f}, {3.5f, 7.8f}, {3.5f, 18.2f}, {11, 18.5f}};
		const float box[4][2] = {{11, 5.5f}, {20.5f, 5.5f}, {20.5f, 18.5f}, {11, 18.5f}};
		strokePoly(spine, 4, true, ox, oy, k, c);
		strokePoly(box, 4, true, ox, oy, k, c);
		strokeArc(ox + 15.75f * k, oy + 12 * k, 2.6f * k, 0, 2 * PI_F, c);
	}
	else if (jc.paused) {
		GX_Begin(GX_TRIANGLES, VTXFMT_DIR, 3);
		emitColor(ox + 7 * k, oy + 4.5f * k, 0, c.r, c.g, c.b, c.a);
		emitColor(ox + 19.5f * k, oy + 12 * k, 0, c.r, c.g, c.b, c.a);
		emitColor(ox + 7 * k, oy + 19.5f * k, 0, c.r, c.g, c.b, c.a);
		GX_End();
	}
	else {
		fillRoundRect(ox + 6.5f * k, oy + 4.5f * k, ox + 10.5f * k, oy + 19.5f * k, 0, c);
		fillRoundRect(ox + 13.5f * k, oy + 4.5f * k, ox + 17.5f * k, oy + 19.5f * k, 0, c);
	}
}

// cubic-bezier(.34, 1.56, .64, 1): the thumbnails' springy settle
static float springBezier(float x)
{
	if (x <= 0) return 0;
	if (x >= 1) return 1;
	float t = x;
	for (int i = 0; i < 8; i++) {
		float bx = 3 * (1 - t) * (1 - t) * t * 0.34f + 3 * (1 - t) * t * t * 0.64f + t * t * t;
		float dx = 3 * (1 - t) * (1 - t) * 0.34f + 6 * (1 - t) * t * (0.64f - 0.34f) + 3 * t * t * (1 - 0.64f);
		if (fabsf(dx) < 1e-5f) break;
		t -= (bx - x) / dx;
		t = sat(t);
	}
	return 3 * (1 - t) * (1 - t) * t * 1.56f + 3 * (1 - t) * t * t * 1.0f + t * t * t;
}

static void texQuad(GXTexObj *tex, float cx, float cy, float size, float rot, GXColor c)
{
	directVtx(true);
	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GX_InvalidateTexAll();
	GX_LoadTexObj(tex, GX_TEXMAP0);
	float h = size / 2, cs = cosf(rot), sn = sinf(rot);
	const float corner[4][4] = {{-1, -1, 0, 0}, {1, -1, 1, 0}, {1, 1, 1, 1}, {-1, 1, 0, 1}};
	GX_Begin(GX_QUADS, VTXFMT_DIR, 4);
	for (int i = 0; i < 4; i++) {
		float x = corner[i][0] * h, y = corner[i][1] * h;
		GX_Position3f32(cx + x * cs - y * sn, cy + x * sn + y * cs, 0);
		GX_Color4u8(c.r, c.g, c.b, c.a);
		GX_TexCoord2f32(corner[i][2], corner[i][3]);
	}
	GX_End();
	colorPipeline();
}

// The original's dock at this width (its max-width: 720px layout), in CSS pixels
#define DOCK_THUMB   42.0f	// a disc's button, padding 4
#define DOCK_GAP     2.0f
#define DOCK_PAD     5.0f
#define DOCK_TOOL    38.0f
#define DOCK_BOTTOM  16.0f
#define DOCK_RADIUS  20.0f
#define RAIL_INSET   10.0f	// the rail's padding, faded out by its mask
#define DOCK_MAX     8		// discs shown at once (the original fits ten)

static void drawDock(const jewel_entry_t *entries, int count, float dt, float progress)
{
	uiPipeline();
	// eight at most: 509px wide, inside what a CRT shows of the picture
	int visible = count < DOCK_MAX ? count : DOCK_MAX;
	float railW = visible * DOCK_THUMB + (visible - 1) * DOCK_GAP + 2 * RAIL_INSET;
	float w = DOCK_PAD * 2 + railW + DOCK_GAP + 5 + DOCK_GAP + 3 * DOCK_TOOL + 2 * DOCK_GAP + 2;
	float h = DOCK_PAD * 2 + DOCK_THUMB + 2;
	// playing, it drops out of the way as the camera runs in
	float drop = 110 * smoothstep(0, 1, jc.zoom * 3);
	float x0 = 320 - w / 2, y1 = 480 - DOCK_BOTTOM + drop, y0 = y1 - h;

	// box-shadow: 0 10px 30px -12px rgba(17,17,20,.18), the frosted panel and its 1px edge
	softRoundRect(x0 + 20, y0 + 10 + 12, x0 + w - 20, y1 + 10 - 12, DOCK_RADIUS, 30, 18, (GXColor) {17, 17, 20, 30});
	if (drawPage) {
		fillRoundRect(x0, y0, x0 + w, y1, DOCK_RADIUS, (GXColor) {17, 17, 20, 20});
		fillRoundRect(x0 + 1, y0 + 1, x0 + w - 1, y1 - 1, DOCK_RADIUS - 1, (GXColor) {255, 255, 255, 240});
	}
	else {
		GXColor edge = THEME_BORDER_DIM, fill = THEME_PANEL_DARK;
		edge.a = 150;
		fillRoundRect(x0, y0, x0 + w, y1, DOCK_RADIUS, edge);
		fillRoundRect(x0 + 1, y0 + 1, x0 + w - 1, y1 - 1, DOCK_RADIUS - 1, fill);
	}
	GXColor ink = dockInk();

	// the rail glides to keep the active disc in view
	int active = JewelCase_Selected();
	if (jc.dockActive != active) {
		jc.dockPrev = jc.dockActive;
		jc.dockActive = active;
		jc.dockChanged = jc.t;
	}
	float first = clampf(active - (visible - 1) / 2.0f, 0, fmaxf(count - visible, 0));
	if (fabsf(jc.railPos - first) > 8) jc.railPos = first;
	jc.railPos += (first - jc.railPos) * (1 - expf(-dt * 8));
	float rx = x0 + 1 + DOCK_PAD + RAIL_INSET, cy = y0 + 1 + DOCK_PAD + DOCK_THUMB / 2;
	float since = (jc.t - jc.dockChanged) / 0.5f, sinceFade = (jc.t - jc.dockChanged) / 0.35f;
	static const float tilts[4] = {-4, 5, -3, 6};
	for (int i = (int)floorf(jc.railPos) - 1; i <= (int)ceilf(jc.railPos) + visible; i++) {
		if (i < 0 || i >= count) continue;
		float slot = i - jc.railPos;
		float cx = rx + slot * (DOCK_THUMB + DOCK_GAP) + DOCK_THUMB / 2;
		// the rail's mask: clear 14px in from either end
		float left = cx - (rx - RAIL_INSET), right = (rx - RAIL_INSET + railW) - cx;
		float vis = sat(fminf(left, right) / 14.0f - 0.5f);
		if (vis <= 0) continue;
		// transform .5s cubic-bezier(.34,1.56,.64,1); opacity and filter .35s ease
		float k = i == active ? springBezier(since) : i == jc.dockPrev ? 1 - springBezier(since) : 0;
		float kf = i == active ? smoothstep(0, 1, sinceFade) : i == jc.dockPrev ? 1 - smoothstep(0, 1, sinceFade) : 0;
		float scale = mixf(0.84f, 1.02f, k), lift = mixf(0, -3, k);
		float tilt = mixf(tilts[i % 4], 0, k) * PI_F / 180.0f;
		// the original fades and greys the discs not chosen; here they keep their colour
		float opacity = vis;
		float size = (DOCK_THUMB - 8) * scale;
		// drop-shadow(0 2px 3px rgba(17,17,20,.18)), the chosen one 0 6px 10px .28
		texQuad(Scene3D_GlowTexture(), cx, cy + lift + mixf(2, 6, kf), size + mixf(6, 20, kf), 0,
			(GXColor) {17, 17, 20, (u8)(mixf(0.18f, 0.28f, kf) * 255 * opacity)});
		artLock();
		art_t *a = findArt(entries[i].id);
		if (a && !a->missing) {
			a->lastUse = frameNo;
			texQuad(&a->thumbObj, cx, cy + lift, size, tilt, (GXColor) {255, 255, 255, (u8)(opacity * 255)});
		}
		else {
			fillCircle(cx, cy + lift, size / 2, (GXColor) {196, 200, 206, (u8)(opacity * 255)});
			fillCircle(cx, cy + lift, size * 0.12f, (GXColor) {255, 255, 255, (u8)(opacity * 255)});
		}
		artUnlock();
		if (i == active) {
			// the bar counting down to the next case: left/right 28%, 2px, 1px from the bottom
			float bx0 = cx - DOCK_THUMB * 0.22f, bx1 = cx + DOCK_THUMB * 0.22f, by = cy + DOCK_THUMB / 2 - 3;
			fillRoundRect(bx0, by, bx1, by + 2, 1, (GXColor) {ink.r, ink.g, ink.b, (u8)(26 * vis * kf)});
			if (progress > 0) fillRoundRect(bx0, by, bx0 + (bx1 - bx0) * progress, by + 2, 1, (GXColor) {ink.r, ink.g, ink.b, (u8)(255 * vis * kf)});
		}
	}
	float x = x0 + 1 + DOCK_PAD + railW + DOCK_GAP;
	fillRoundRect(x + 2, y0 + 1 + 8, x + 3, y1 - 1 - 8, 0, (GXColor) {ink.r, ink.g, ink.b, drawPage ? 20 : 50});
	x += 5 + DOCK_GAP;
	// background .25s ease
	float ease = 1 - expf(-dt * 12);
	jc.toolOn[0] += ((jc.spinOn ? 1 : 0) - jc.toolOn[0]) * ease;
	jc.toolOn[1] += ((JewelCase_IsOpen() ? 1 : 0) - jc.toolOn[1]) * ease;
	drawTool(0, x + DOCK_TOOL / 2, cy, DOCK_TOOL, jc.toolOn[0]);
	x += DOCK_TOOL + DOCK_GAP;
	drawTool(1, x + DOCK_TOOL / 2, cy, DOCK_TOOL, jc.toolOn[1]);
	x += DOCK_TOOL + DOCK_GAP;
	drawTool(2, x + DOCK_TOOL / 2, cy, DOCK_TOOL, 0);
	GX_SetLineWidth(6, GX_TO_ZERO);
}

/* Stars ------------------------------------------------------------------------- */

// Where a rod mirrors one of the studio's hot lamps the original throws a star: a
// glint that streaks out in eight directions. On the white page added light can't
// show, so it tints what it lies over a cool blue, white-hot in the middle.
#define MAX_STARS 2
#define GLINT_TOL 0.3f
#define GLINT_MIN 0.3f
typedef struct { float x, y, k; } star_t;

static void findGlints(const rodmesh_t *m, Mtx mv, float scale, float fade, star_t *stars, int *n)
{
	Mtx44 proj;
	guPerspective(proj, FOV_Y, 640.0f / 480.0f, 5.0f, 4000.0f);
	float os = UI_GetOverscan(), lift = liftNow();
	v3 L[4];
	for (int l = 0; l < 4; l++) L[l] = vnorm(V3(studioLamps[l].x, studioLamps[l].y, 1.0f));
	const float cosTol = cosf(GLINT_TOL);
	// the case is small in the picture: one direction to the camera serves all of it
	v3 V = vnorm(V3(-mv[0][3], -mv[1][3], -mv[2][3]));
	for (int i = 0; i < m->glintEnd; i++) {
		const float *nn = m->nrm + i * 3;
		v3 N;
		xformDir(mv, V3(nn[0], nn[1], nn[2]), &N);
		N = vmul(N, 1.0f / scale);
		float cosv = vdot(N, V);
		if (cosv <= 0.05f) continue;
		v3 R = vreflect(vmul(V, -1), N);
		float best = 0;
		for (int l = 0; l < 4; l++) {
			float d = vdot(R, L[l]);
			if (d < cosTol) continue;
			float ang = acosf(fminf(d, 1.0f));
			// the profile is sampled every 20-odd degrees: allow for that, but fall off fast
			{
				float k = 1 - ang / GLINT_TOL;
				best = fmaxf(best, k * k * k * k * fresnel(cosv, IOR) * 16.0f);
			}
		}
		if (best < GLINT_MIN) continue;
		best *= fade;
		const float *pp = m->pos + i * 3;
		v3 P;
		xform(mv, V3(pp[0], pp[1], pp[2]), &P);
		// to the screen
		float cx = proj[0][0] * os * P.x, cw = -P.z;
		float cy = proj[1][1] * os * P.y + lift * os * cw;
		float sx = (cx / cw * 0.5f + 0.5f) * 640, sy = (0.5f - cy / cw * 0.5f) * 480;
		// keep the strongest few, apart from each other
		int j;
		for (j = 0; j < *n; j++) {
			float dx = stars[j].x - sx, dy = stars[j].y - sy;
			if (dx * dx + dy * dy < 30 * 30) break;
		}
		if (j < *n) {
			if (best > stars[j].k) stars[j] = (star_t) {sx, sy, best};
			continue;
		}
		if (*n < MAX_STARS) stars[(*n)++] = (star_t) {sx, sy, best};
		else {
			int w = 0;
			for (int q = 1; q < *n; q++) if (stars[q].k < stars[w].k) w = q;
			if (best > stars[w].k) stars[w] = (star_t) {sx, sy, best};
		}
	}
}

static void streak(float x, float y, float dx, float dy, float len, float width, GXColor c)
{
	float nx = -dy * width, ny = dx * width;
	GX_Begin(GX_TRIANGLEFAN, VTXFMT_DIR, 6);
	emitColor(x, y, 0, c.r, c.g, c.b, c.a);
	emitColor(x + nx, y + ny, 0, c.r, c.g, c.b, c.a / 3);
	emitColor(x + dx * len, y + dy * len, 0, c.r, c.g, c.b, 0);
	emitColor(x - nx, y - ny, 0, c.r, c.g, c.b, c.a / 3);
	emitColor(x - dx * len, y - dy * len, 0, c.r, c.g, c.b, 0);
	emitColor(x + nx, y + ny, 0, c.r, c.g, c.b, c.a / 3);
	GX_End();
}

static void drawStars(const star_t *stars, int n)
{
	if (!n) return;
	uiPipeline();
	for (int i = 0; i < n; i++) {
		// softer over the dark backdrop, where they stand out more
		float k = sat(stars[i].k) * (drawPage ? 1.0f : 0.6f);
		u8 a = (u8)(sat(k * 1.6f) * 0.75f * 255);
		GXColor blue = {107, 158, 255, a};
		// the original's streaks run longest up and down
		float len = 16 + 30 * k;
		streak(stars[i].x, stars[i].y, 0, 1, len, 0.8f, blue);
		streak(stars[i].x, stars[i].y, 1, 0, len * 0.45f, 0.8f, blue);
		streak(stars[i].x, stars[i].y, 0.7071f, 0.7071f, len * 0.2f, 0.6f, blue);
		streak(stars[i].x, stars[i].y, 0.7071f, -0.7071f, len * 0.2f, 0.6f, blue);
		texQuad(Scene3D_GlowTexture(), stars[i].x, stars[i].y, 8 + 8 * k, 0, (GXColor) {130, 175, 255, a});
		// the white-hot middle
		GXColor white = {255, 255, 255, (u8)(sat(k * 1.6f) * 255)};
		streak(stars[i].x, stars[i].y, 0, 1, len * 0.35f, 0.6f, white);
		streak(stars[i].x, stars[i].y, 1, 0, len * 0.15f, 0.6f, white);
		fillCircle(stars[i].x, stars[i].y, 0.9f + k, (GXColor) {255, 255, 255, (u8)(k * 255)});
	}
}

/* Set-up and the frame --------------------------------------------------------- */

static bool initialised;

static bool prepared;

// Everything that is only arithmetic: the studio's matcap (a fraction of a second
// on the GameCube) and the meshes. Run on the main thread when the view opens.
static void jewelPrepare(void)
{
	if (prepared) return;
	initSrgb();
	initCurves();
	initOutlines();
	for (int i = 0; i <= DISC_SEGS; i++) {
		discCos[i] = cosf(i * 2 * PI_F / DISC_SEGS);
		discSin[i] = sinf(i * 2 * PI_F / DISC_SEGS);
	}
	buildMatcap();
	buildRoomTable();
	buildTrayRods(&trayRods);
	buildLidRods(&lidRods);
	buildSheets();
	buildSheen();
	prepared = true;
}

// The display lists have to be recorded on the thread that owns GX
static void jewelInit(void)
{
	jewelPrepare();
	initialised = true;
	rodCompile(&trayRods);
	rodCompile(&lidRods);

	// the camera stands back far enough for the case to fill 60% of the height
	float c = tanf(14.0f * PI_F / 180.0f), aspect = 640.0f / 480.0f;
	camDist = fmaxf(146.0f / 0.6f / 2.0f / c, 94.9f / (c * aspect)) / viewFill;
	openScale = fminf(1.04f, 2 * camDist * c * aspect / 328.5f);
	guMtxIdentity(view);
	view[2][3] = -camDist;
}

typedef struct { int caseIdx; bool lid; float depth; } part_t;

void JewelCase_Draw(const jewel_entry_t *entries, int count)
{
	if (!initialised) jewelInit();
	artLock();
	if (jc.count != count) {
		jc.count = count;
		if (jc.cur >= count) jc.cur = count ? count - 1 : 0;
		jc.tr = false;
	}
	frameNo++;
	float dt = Scene3D_FrameDelta();
	update(dt);

	// which cases are on stage, and how
	shown_t cases[2];
	int n = 0;
	float t = jc.t;
	if (count > 0) {
		if (jc.tr) {
			float v = sat((t - jc.trStart) / TRANSITION);
			// the original's rise and fall run across: its y keys on x, its pitch as yaw
			float g = jc.trDir;
			shown_t *c = &cases[n++];
			c->index = jc.trFrom;
			c->pose = casePose(jc.trFrom, t, jc.trPhase, jc.trPhaseAt);
			const kf_t fy[] = {{0, 0, NULL}, {.48f, -(.8f * g * 146), NULL}, {1, -(.12f * g * 146), NULL}};
			const kf_t fz[] = {{0, 0, NULL}, {.45f, -46, NULL}, {1, -82, easeIn}};
			const kf_t fs[] = {{0, 1, NULL}, {1, .62f, easeIn}};
			const kf_t fr[] = {{0, 0, NULL}, {.5f, .2f * g, NULL}, {1, .08f * g, NULL}};
			const kf_t fp[] = {{0, 0, NULL}, {.45f, .3f * g, NULL}, {1, 0, NULL}};
			const kf_t ff[] = {{0, 1, NULL}, {.55f, .8f, NULL}, {1, 0, easeIn}};
			c->pose.x += keys(v, fy, 3);
			c->pose.z += keys(v, fz, 3);
			c->pose.s *= keys(v, fs, 2);
			c->pose.roll += keys(v, fr, 3);
			c->pose.yaw -= keys(v, fp, 3);
			c->pose.fade = keys(v, ff, 3);
			c = &cases[n++];
			c->index = jc.trTo;
			c->pose = restPose(t, 1.7f * jc.trTo + 0.4f);
			const kf_t ty[] = {{0, .12f * g * 146, NULL}, {.42f, .9f * g * 146, easeOut}, {1, 0, easeInOut}};
			const kf_t tz[] = {{0, -55, NULL}, {.25f, -32, easeOut}, {.5f, 27, NULL}, {1, 0, NULL}};
			const kf_t ts[] = {{0, .62f, NULL}, {.42f, .9f, easeOut}, {1, 1, easeOut}};
			const kf_t tr[] = {{0, -.28f * g, NULL}, {.42f, .1f * g, NULL}, {1, 0, easeSpring}};
			const kf_t tp[] = {{0, .1f * g, NULL}, {.42f, -.4f * g, NULL}, {.8f, .14f * g, NULL}, {1, 0, NULL}};
			c->pose.x += keys(v, ty, 3);
			c->pose.z += keys(v, tz, 4);
			c->pose.s *= keys(v, ts, 3);
			c->pose.roll += keys(v, tr, 3);
			c->pose.yaw -= keys(v, tp, 4);
		}
		else {
			shown_t *c = &cases[n++];
			c->index = jc.cur;
			c->pose = casePose(jc.cur, t, jc.phase, jc.phaseAt);
		}
	}
	for (int i = 0; i < n; i++) {
		placeCase(&cases[i]);
		cases[i].art = findArt(entries[cases[i].index].id);
		if (cases[i].art) cases[i].art->lastUse = frameNo;
	}
	// coming in under black: wait for the shown case's art (or give the slowest
	// of devices a second at it), hold a beat past ready, then lift the cover
	if (jc.intro > 0) {
		bool ready = n == 0;
		for (int i = 0; i < n; i++) if (cases[i].art) ready = true;
		float held = t - jc.introAt;
		if (jc.intro >= 1.0f) {
			if ((ready && held >= INTRO_MIN) || held >= INTRO_MAX) jc.intro = 1.0f - 1e-3f;
		}
		else jc.intro = fmaxf(0.0f, jc.intro - dt / INTRO_FADE);
	}
	// playing: the camera runs in to the disc
	guMtxIdentity(view);
	view[2][3] = -camDist;
	if (jc.zoom > 0 && n == 1) {
		float k = jc.zoom;
		Mtx *m = &cases[0].mCase;
		view[0][3] = -k * (*m)[0][3];
		view[1][3] = -k * (*m)[1][3];
		view[2][3] = -mixf(camDist, (*m)[2][3] + LAUNCH_NEAR, k);
		placeCase(&cases[0]);
	}
	float progress = jc.tr ? 0 : sat(jc.clock / CYCLE);

	// the white page
	uiPipeline();
	{
		Mtx id;
		guMtxIdentity(id);
		GX_LoadPosMtxImm(id, GX_PNMTX0);
		Mtx44 o;
		guOrtho(o, 0, 480, 0, 640, 0, 1);
		GX_LoadProjectionMtx(o, GX_ORTHOGRAPHIC);
		GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
		// at the far plane, so it also clears the depth buffer for the case. Over
		// the backdrop there's nothing to do: the copy to the screen clears depth
		// each frame (FrameBufferMagic turns depth writes on for it, which the
		// clear needs) and Swiss's own drawing never writes it
		if (drawPage) {
			GX_SetZMode(GX_ENABLE, GX_ALWAYS, GX_TRUE);
			GX_Begin(GX_QUADS, VTXFMT_DIR, 4);
			emitColor(0, 0, -0.9999f, 255, 255, 255, 255);
			emitColor(640, 0, -0.9999f, 255, 255, 255, 255);
			emitColor(640, 480, -0.9999f, 255, 255, 255, 255);
			emitColor(0, 480, -0.9999f, 255, 255, 255, 255);
			GX_End();
			GX_SetZMode(GX_DISABLE, GX_ALWAYS, GX_FALSE);
		}
	}

	loadProjection();
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	// the shadows only darken the white page: on Swiss's dark backdrop they come to
	// a few levels at most, not worth their thousands of points
	if (drawPage) drawShadows(cases, n);

	// the discs, then the plastic from the back forwards
	for (int i = 0; i < n; i++) drawDisc(&cases[i]);
	part_t parts[4];
	int np = 0;
	for (int i = 0; i < n; i++) {
		parts[np++] = (part_t) {i, false, cases[i].depthTray};
		parts[np++] = (part_t) {i, true, cases[i].depthLid};
	}
	for (int i = 1; i < np; i++) {
		for (int j = i; j > 0 && parts[j].depth < parts[j - 1].depth; j--) {
			part_t tmp = parts[j]; parts[j] = parts[j - 1]; parts[j - 1] = tmp;
		}
	}
	for (int i = 0; i < np; i++) {
		shown_t *c = &cases[parts[i].caseIdx];
		if (parts[i].lid) {
			drawRods(&lidRods, c->mvLid, c->scale, c->pose.fade);
			drawSheets(&lidSheet, 1, c->mvLid, c->scale, c->pose.fade);
			drawSheen(c->mvLid, 5.0f, c->pose.fade);
		}
		else {
			drawRods(&trayRods, c->mvCase, c->scale, c->pose.fade);
			drawSheets(traySheets, traySheetCount, c->mvCase, c->scale, c->pose.fade);
		}
	}
	GX_SetZMode(GX_DISABLE, GX_ALWAYS, GX_FALSE);
	star_t stars[MAX_STARS];
	int nstars = 0;
	for (int i = 0; i < n; i++) {
		findGlints(&trayRods, cases[i].mvCase, cases[i].scale, cases[i].pose.fade, stars, &nstars);
		findGlints(&lidRods, cases[i].mvLid, cases[i].scale, cases[i].pose.fade, stars, &nstars);
	}
	artUnlock();
	drawStars(stars, nstars);

	if (count > 0 && showDock) drawDock(entries, count, dt, progress);

	// and to black: the launch fade, or the cover held while coming in
	float cover = fmaxf(jc.fade, jc.intro);
	if (cover > 0.002f) {
		uiPipeline();
		Mtx id;
		guMtxIdentity(id);
		GX_LoadPosMtxImm(id, GX_PNMTX0);
		Mtx44 o;
		guOrtho(o, 0, 480, 0, 640, 0, 1);
		GX_LoadProjectionMtx(o, GX_ORTHOGRAPHIC);
		GX_SetZMode(GX_DISABLE, GX_ALWAYS, GX_FALSE);
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
		u8 a = (u8)(smoothstep(0, 1, sat(cover)) * 255);
		GX_Begin(GX_QUADS, VTXFMT_DIR, 4);
		emitColor(-8, -8, 0, 0, 0, 0, a);
		emitColor(648, -8, 0, 0, 0, 0, a);
		emitColor(648, 488, 0, 0, 0, 0, a);
		emitColor(-8, 488, 0, 0, 0, 0, a);
		GX_End();
	}

	// back to the state the 2D UI expects
	GX_SetNumTevStages(1);
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_1);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_InvalidateTexAll();
	UI_LoadProjection();
}
