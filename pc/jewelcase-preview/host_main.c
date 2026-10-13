/* Preview driver: runs jewelcase.c against the software GX and saves frames */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gccore.h"
#include "deviceHandler.h"
#include "jewelcase.h"

void gx_clear(float r, float g, float b);
void gx_save_ppm(const char *path);
void gx_backdrop(void);
void gx_copy_clear(void);
extern unsigned long gx_statVerts, gx_statPixels, gx_statPrims;
extern int gx_noRaster;

/* scene3d.h */
static float frameDt = 1.0f / 60.0f;
float Scene3D_FrameDelta(void) { return frameDt; }
float Scene3D_Time(void) { return 0; }
float UI_GetOverscan(void) { return 1.0f; }
void UI_ApplyTransform(Mtx local, Mtx out) { guMtxCopy(local, out); }
void UI_LoadProjection(void) { Mtx44 o; guOrtho(o, 0, 480, 0, 640, 0, 1); GX_LoadProjectionMtx(o, GX_ORTHOGRAPHIC); }
static u8 glowData[64 * 64];
static GXTexObj glowTex;
GXTexObj *Scene3D_GlowTexture(void)
{
	static int done;
	if (!done) {
		done = 1;
		for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
			float dx = (x - 31.5f) / 32.0f, dy = (y - 31.5f) / 32.0f, d = 1.0f - sqrtf(dx * dx + dy * dy);
			glowData[((y / 4) * 8 + (x / 8)) * 32 + (y % 4) * 8 + (x % 8)] = (u8)(d <= 0 ? 0 : d * d * 255);
		}
		GX_InitTexObj(&glowTex, glowData, 64, 64, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	}
	return &glowTex;
}

/* files.h and a device reading from a host folder */
size_t concat_path(char *p, const char *d, const char *b) { return sprintf(p, "%s%s", d, b); }
static const char *sdRoot;
static s32 hStat(file_handle *f)
{
	char path[2048];
	snprintf(path, sizeof(path), "%s%s", sdRoot, f->name + 4);
	FILE *fp = fopen(path, "rb");
	if (!fp) return -1;
	fseek(fp, 0, SEEK_END);
	f->size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	f->fp = fp;
	return 0;
}
static s64 hSeek(file_handle *f, s64 o, u32 w) { (void)w; return f->fp ? fseek(f->fp, o, SEEK_SET) : -1; }
static s32 hRead(file_handle *f, void *b, u32 n) { return f->fp ? (s32)fread(b, 1, n, f->fp) : -1; }
static s32 hClose(file_handle *f) { if (f->fp) fclose(f->fp); f->fp = NULL; return 0; }
static file_handle root = {.name = "sd:/"};
static DEVICEHANDLER_INTERFACE hostSd = {"SD Card", &root, hStat, hSeek, hRead, hClose};
DEVICEHANDLER_INTERFACE *devices[8] = {&hostSd};

void probe_dump_matcap(const char *path);
void probe_record_mode(void);
void probe_counts(void);
void probe_sheet_counts(void);
int main(int argc, char **argv)
{
	if (argc > 2 && !strcmp(argv[1], "matcap")) { probe_dump_matcap(argv[2]); return 0; }
	// usage: preview out_dir id,id,... [t=action...]
	//   actions: open, play, done, close, next, prev, spin, pause, drag:x:y, undrag, shot:label
	sdRoot = getenv("JC_SD") ? getenv("JC_SD") : "./sd/";
	if (getenv("JC_RECORD")) probe_record_mode();
	gx_noRaster = getenv("JC_NORASTER") != NULL;
	if (getenv("JC_COUNTS")) { probe_counts(); probe_sheet_counts(); return 0; }
	if (argc < 3) { fprintf(stderr, "usage: preview out_dir id,... [t=action...]\n"); return 1; }
	const char *out = argv[1];
	char ids[64][8];
	int n = 0;
	for (char *tok = strtok(argv[2], ","); tok && n < 64; tok = strtok(NULL, ",")) { snprintf(ids[n], 8, "%.7s", tok); n++; }
	jewel_entry_t entries[64];
	memset(entries, 0, sizeof(entries));
	for (int i = 0; i < n; i++) strcpy(entries[i].id, ids[i]);
	JewelCase_Enter(n, 0);
	for (int i = 0; i < n; i++) JewelCase_LoadArt(ids[i]);

	float tEnd = 0;
	for (int a = 3; a < argc; a++) tEnd = fmaxf(tEnd, atof(argv[a]));
	int frames = (int)ceilf(tEnd * 60) + 1;
	// as GX_Init leaves it: depth cleared
	gx_clear(0, 0, 0);
	int next = 3;
	for (int f = 0; f <= frames; f++) {
		float t = f / 60.0f;
		bool shoot = false;
		char label[64] = "";
		while (next < argc && atof(argv[next]) <= t + 1e-4f) {
			const char *eq = strchr(argv[next], '=');
			if (!eq) { next++; continue; }
			const char *act = eq + 1;
			if (!strcmp(act, "open")) JewelCase_SetOpen(true);
			else if (!strcmp(act, "play")) JewelCase_Launch();
			else if (!strcmp(act, "done")) printf("launch done: %d\n", JewelCase_LaunchDone());
			else if (!strcmp(act, "close")) JewelCase_SetOpen(false);
			else if (!strcmp(act, "next")) JewelCase_Step(1);
			else if (!strcmp(act, "prev")) JewelCase_Step(-1);
			else if (!strcmp(act, "spin")) JewelCase_ToggleSpin();
			else if (!strcmp(act, "pause")) JewelCase_TogglePause();
			else if (!strncmp(act, "drag:", 5)) { float x, y; sscanf(act + 5, "%f:%f", &x, &y); JewelCase_SetDrag(x, y, true); }
			else if (!strcmp(act, "undrag")) JewelCase_SetDrag(0, 0, false);
			else if (!strncmp(act, "shot:", 5)) { shoot = true; snprintf(label, sizeof(label), "%.60s", act + 5); }
			next++;
		}
		gx_backdrop();
		gx_statVerts = gx_statPixels = gx_statPrims = 0;
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		JewelCase_Draw(entries, n);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		// the rest of Swiss's frame: its 2D drawing leaves depth writes off, then
		// FrameBufferMagic turns them on for the copy so it clears depth
		// (JC_NOZCLEAR: as it was before, depth never cleared)
		GX_SetZMode(GX_DISABLE, GX_ALWAYS, GX_FALSE);
		if (!getenv("JC_NOZCLEAR")) GX_SetZMode(GX_ENABLE, GX_ALWAYS, GX_TRUE);
		if (getenv("JC_STATS")) printf("t %.2f us %.0f verts %lu prims %lu pixels %lu\n", t, (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3,
			gx_statVerts, gx_statPrims, gx_statPixels);
		if (shoot) {
			char path[512];
			snprintf(path, sizeof(path), "%s/%s.ppm", out, label);
			gx_save_ppm(path);
			printf("saved %s at %.2fs\n", path, t);
		}
		gx_copy_clear();
	}
	return 0;
}
