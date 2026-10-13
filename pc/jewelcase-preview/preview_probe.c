/* Builds jewelcase.c into the preview and reaches a few of its statics */
#include "../../cube/swiss/source/gui/jewelcase.c"

// The rod matcap, colour on the left and see-through share on the right
void probe_dump_matcap(const char *path)
{
	initSrgb();
	if (!matcapData) buildMatcap();
	FILE *f = fopen(path, "wb");
	fprintf(f, "P6\n%d %d\n255\n", MATCAP_SIZE * 2, MATCAP_SIZE);
	for (int y = 0; y < MATCAP_SIZE; y++) {
		for (int x = 0; x < MATCAP_SIZE * 2; x++) {
			int c[4] = {0, 0, 0, 0};
			getRGBA8(matcapData, MATCAP_SIZE, x % MATCAP_SIZE, y, c);
			if (x < MATCAP_SIZE) { fputc(c[0], f); fputc(c[1], f); fputc(c[2], f); }
			else { fputc(c[3], f); fputc(c[3], f); fputc(c[3], f); }
		}
	}
	fclose(f);
}

// As the original's ?record mode: its white page, no dock, the picture not lifted for it
void probe_record_mode(void) { drawPage = true; viewLift = 0; viewFill = 1; showDock = false; }

void probe_counts(void)
{
	if (!initialised) jewelInit();
	printf("tray rods: %d verts, display list %u bytes; lid rods: %d verts, display list %u bytes\n",
		trayRods.count, trayRods.dlSize, lidRods.count, lidRods.dlSize);
}

void probe_sheet_counts(void)
{
	if (!prepared) jewelPrepare();
	for (int i = 0; i < traySheetCount; i++) printf("tray sheet %d: %d segs\n", i, traySheets[i].segs);
	printf("lid sheet: %d segs\n", lidSheet.segs);
}
