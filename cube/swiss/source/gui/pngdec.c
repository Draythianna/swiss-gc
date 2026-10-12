/* -----------------------------------------------------------
      pngdec.c - minimal PNG decoder on top of zlib
   ----------------------------------------------------------- */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "pngdec.h"

#define PNG_MAX_DIM 1024

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u8 paeth(int a, int b, int c)
{
	int p = a + b - c;
	int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	if (pa <= pb && pa <= pc) return a;
	return pb <= pc ? b : c;
}

u8 *PNG_Decode(const u8 *data, u32 size, int *outW, int *outH)
{
	static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	if (size < 33 || memcmp(data, sig, 8)) return NULL;

	u32 w = 0, h = 0;
	int depth = 0, type = -1, interlace = 0;
	u8 palette[256 * 4];
	memset(palette, 0xFF, sizeof(palette));

	u8 *rgba = NULL, *raw = NULL;
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	bool zinit = false, done = false;
	int channels = 0;
	u32 rawSize = 0;

	u32 pos = 8;
	while (pos + 12 <= size && !done) {
		u32 len = be32(data + pos);
		const u8 *tag = data + pos + 4;
		const u8 *chunk = data + pos + 8;
		if (len > size - pos - 12) break;

		if (!memcmp(tag, "IHDR", 4) && len >= 13) {
			w = be32(chunk);
			h = be32(chunk + 4);
			depth = chunk[8];
			type = chunk[9];
			interlace = chunk[12];
			if (!w || !h || w > PNG_MAX_DIM || h > PNG_MAX_DIM || depth != 8 || interlace) goto fail;
			switch (type) {
				case 0: channels = 1; break;
				case 2: channels = 3; break;
				case 3: channels = 1; break;
				case 4: channels = 2; break;
				case 6: channels = 4; break;
				default: goto fail;
			}
			rawSize = (w * channels + 1) * h;
			raw = malloc(rawSize);
			if (!raw || inflateInit(&zs) != Z_OK) goto fail;
			zinit = true;
			zs.next_out = raw;
			zs.avail_out = rawSize;
		}
		else if (!memcmp(tag, "PLTE", 4)) {
			for (u32 i = 0; i < len / 3 && i < 256; i++) {
				palette[i * 4 + 0] = chunk[i * 3 + 0];
				palette[i * 4 + 1] = chunk[i * 3 + 1];
				palette[i * 4 + 2] = chunk[i * 3 + 2];
			}
		}
		else if (!memcmp(tag, "tRNS", 4) && type == 3) {
			for (u32 i = 0; i < len && i < 256; i++) palette[i * 4 + 3] = chunk[i];
		}
		else if (!memcmp(tag, "IDAT", 4)) {
			if (!zinit) goto fail;
			zs.next_in = (Bytef *)chunk;
			zs.avail_in = len;
			while (zs.avail_in && zs.avail_out) {
				int ret = inflate(&zs, Z_NO_FLUSH);
				if (ret == Z_STREAM_END) break;
				if (ret != Z_OK) goto fail;
			}
		}
		else if (!memcmp(tag, "IEND", 4)) {
			done = true;
		}
		pos += len + 12;
	}
	if (!zinit || zs.avail_out) goto fail;
	inflateEnd(&zs);
	zinit = false;

	// Undo the per row filters in place
	u32 stride = w * channels;
	for (u32 y = 0; y < h; y++) {
		u8 *row = raw + y * (stride + 1);
		u8 filter = row[0];
		u8 *cur = row + 1;
		u8 *prev = y ? raw + (y - 1) * (stride + 1) + 1 : NULL;
		for (u32 x = 0; x < stride; x++) {
			int a = x >= (u32)channels ? cur[x - channels] : 0;
			int b = prev ? prev[x] : 0;
			int c = (prev && x >= (u32)channels) ? prev[x - channels] : 0;
			switch (filter) {
				case 0: break;
				case 1: cur[x] += a; break;
				case 2: cur[x] += b; break;
				case 3: cur[x] += (a + b) >> 1; break;
				case 4: cur[x] += paeth(a, b, c); break;
				default: goto fail;
			}
		}
	}

	rgba = malloc(w * h * 4);
	if (!rgba) goto fail;
	for (u32 y = 0; y < h; y++) {
		const u8 *src = raw + y * (stride + 1) + 1;
		u8 *dst = rgba + y * w * 4;
		for (u32 x = 0; x < w; x++, dst += 4) {
			switch (type) {
				case 0: dst[0] = dst[1] = dst[2] = src[x]; dst[3] = 255; break;
				case 2: memcpy(dst, src + x * 3, 3); dst[3] = 255; break;
				case 3: memcpy(dst, palette + src[x] * 4, 4); break;
				case 4: dst[0] = dst[1] = dst[2] = src[x * 2]; dst[3] = src[x * 2 + 1]; break;
				case 6: memcpy(dst, src + x * 4, 4); break;
			}
		}
	}
	free(raw);
	*outW = w;
	*outH = h;
	return rgba;

fail:
	if (zinit) inflateEnd(&zs);
	free(raw);
	free(rgba);
	return NULL;
}
