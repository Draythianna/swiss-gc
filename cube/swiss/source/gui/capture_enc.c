/* -----------------------------------------------------------
      capture_enc.c - Image and video encoders for screen capture

      A small baseline JPEG encoder (for MJPEG recordings), a PNG
      writer (for screenshots) and the AVI container pieces.
      All of it reads YUY2 a byte at a time, so it is endian
      neutral and runs the same on the GameCube and a PC.
   ----------------------------------------------------------- */

#include <stdlib.h>
#include <string.h>
#include <libdeflate.h>
#include "capture_enc.h"

static inline int clamp255(int v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* ---------------------------------------------------------------------------
 * Scaling
 * ------------------------------------------------------------------------- */

void Enc_HalveYUY2(const uint8_t *src, int width, int height, int stride, int halveV, uint8_t *dst)
{
	int outW = width / 2, outH = halveV ? height / 2 : height;
	for (int y = 0; y < outH; y++) {
		const uint8_t *a = src + (halveV ? y * 2 : y) * stride;
		const uint8_t *b = halveV ? a + stride : a;
		uint8_t *d = dst + y * outW * 2;
		// Every 8 source bytes (4 pixels) make 4 output bytes (2 pixels)
		for (int x = 0; x + 1 < outW; x += 2, a += 8, b += 8, d += 4) {
			d[0] = (a[0] + a[2] + b[0] + b[2] + 2) >> 2;
			d[1] = (a[1] + a[5] + b[1] + b[5] + 2) >> 2;
			d[2] = (a[4] + a[6] + b[4] + b[6] + 2) >> 2;
			d[3] = (a[3] + a[7] + b[3] + b[7] + 2) >> 2;
		}
	}
}

/* ---------------------------------------------------------------------------
 * JPEG
 * ------------------------------------------------------------------------- */

static const uint8_t zigzag[64] = {
	 0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static const uint8_t stdLumaQuant[64] = {
	16, 11, 10, 16, 24, 40, 51, 61,
	12, 12, 14, 19, 26, 58, 60, 55,
	14, 13, 16, 24, 40, 57, 69, 56,
	14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68,109,103, 77,
	24, 35, 55, 64, 81,104,113, 92,
	49, 64, 78, 87,103,121,120,101,
	72, 92, 95, 98,112,100,103, 99
};

static const uint8_t stdChromaQuant[64] = {
	17, 18, 24, 47, 99, 99, 99, 99,
	18, 21, 26, 66, 99, 99, 99, 99,
	24, 26, 56, 99, 99, 99, 99, 99,
	47, 66, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99
};

// Standard Huffman tables (ITU T.81 Annex K.3)
static const uint8_t dcLumaBits[16]   = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t dcChromaBits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t dcVals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t acLumaBits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t acLumaVals[162] = {
	0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
	0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
	0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
	0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
	0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
	0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
	0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
	0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
	0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
	0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
	0xf9,0xfa
};

static const uint8_t acChromaBits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t acChromaVals[162] = {
	0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
	0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
	0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
	0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
	0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
	0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
	0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
	0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
	0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
	0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
	0xf9,0xfa
};

typedef struct {
	uint16_t code[256];
	uint8_t size[256];
} HuffTable;

static HuffTable dcLuma, dcChroma, acLuma, acChroma;
static uint8_t fullRangeY[256], fullRangeC[256];
static int tablesReady;

static void buildHuff(HuffTable *t, const uint8_t bits[16], const uint8_t *vals)
{
	uint16_t code = 0;
	int k = 0;
	for (int len = 1; len <= 16; len++) {
		for (int i = 0; i < bits[len - 1]; i++, k++) {
			t->code[vals[k]] = code++;
			t->size[vals[k]] = len;
		}
		code <<= 1;
	}
}

static void initTables(void)
{
	if (tablesReady) return;
	buildHuff(&dcLuma, dcLumaBits, dcVals);
	buildHuff(&dcChroma, dcChromaBits, dcVals);
	buildHuff(&acLuma, acLumaBits, acLumaVals);
	buildHuff(&acChroma, acChromaBits, acChromaVals);
	// The XFB is BT.601 limited range, JFIF is full range
	for (int i = 0; i < 256; i++) {
		fullRangeY[i] = clamp255(((i - 16) * 255 + 109) / 219);
		fullRangeC[i] = clamp255(128 + ((i - 128) * 255 + (i >= 128 ? 112 : -112)) / 224);
	}
	tablesReady = 1;
}

typedef struct {
	uint8_t *p, *end;
	uint32_t acc;
	int bits;
	int overflow;
} BitWriter;

static inline void putByte(BitWriter *w, uint8_t c)
{
	if (w->p < w->end) *w->p++ = c;
	else w->overflow = 1;
}

static inline void putBits(BitWriter *w, uint32_t code, int len)
{
	w->acc = (w->acc << len) | (code & ((1u << len) - 1));
	w->bits += len;
	while (w->bits >= 8) {
		uint8_t c = w->acc >> (w->bits - 8);
		putByte(w, c);
		if (c == 0xFF) putByte(w, 0);	// byte stuffing
		w->bits -= 8;
	}
	w->acc &= (1u << w->bits) - 1;
}

static void putMarker(BitWriter *w, uint8_t marker, int length)
{
	putByte(w, 0xFF);
	putByte(w, marker);
	if (length) {
		putByte(w, length >> 8);
		putByte(w, length);
	}
}

static void putHuffTable(BitWriter *w, int cls, const uint8_t bits[16], const uint8_t *vals)
{
	int count = 0;
	for (int i = 0; i < 16; i++) count += bits[i];
	putMarker(w, 0xC4, 2 + 1 + 16 + count);
	putByte(w, cls);
	for (int i = 0; i < 16; i++) putByte(w, bits[i]);
	for (int i = 0; i < count; i++) putByte(w, vals[i]);
}

// AAN forward DCT on one 8x8 block in place (output is scaled, folded into the quantiser)
static void fdct(float *d)
{
	for (int pass = 0; pass < 2; pass++) {
		int step = pass ? 8 : 1, next = pass ? 1 : 8;
		for (int i = 0; i < 8; i++) {
			float *p = d + i * next;
			float t0 = p[0*step] + p[7*step], t7 = p[0*step] - p[7*step];
			float t1 = p[1*step] + p[6*step], t6 = p[1*step] - p[6*step];
			float t2 = p[2*step] + p[5*step], t5 = p[2*step] - p[5*step];
			float t3 = p[3*step] + p[4*step], t4 = p[3*step] - p[4*step];

			float t10 = t0 + t3, t13 = t0 - t3;
			float t11 = t1 + t2, t12 = t1 - t2;
			p[0*step] = t10 + t11;
			p[4*step] = t10 - t11;
			float z1 = (t12 + t13) * 0.707106781f;
			p[2*step] = t13 + z1;
			p[6*step] = t13 - z1;

			t10 = t4 + t5;
			t11 = t5 + t6;
			t12 = t6 + t7;
			float z5 = (t10 - t12) * 0.382683433f;
			float z2 = 0.541196100f * t10 + z5;
			float z4 = 1.306562965f * t12 + z5;
			float z3 = t11 * 0.707106781f;
			float z11 = t7 + z3, z13 = t7 - z3;
			p[5*step] = z13 + z2;
			p[3*step] = z13 - z2;
			p[1*step] = z11 + z4;
			p[7*step] = z11 - z4;
		}
	}
}

static void encodeBlock(BitWriter *w, float *blk, const float *divisors, int *dcPred, const HuffTable *dc, const HuffTable *ac)
{
	int q[64];
	fdct(blk);
	for (int i = 0; i < 64; i++) {
		float v = blk[zigzag[i]] * divisors[zigzag[i]];
		q[i] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
	}

	int diff = q[0] - *dcPred;
	*dcPred = q[0];
	int mag = diff < 0 ? -diff : diff, nbits = 0;
	while (mag) { nbits++; mag >>= 1; }
	putBits(w, dc->code[nbits], dc->size[nbits]);
	if (nbits) putBits(w, diff < 0 ? diff - 1 : diff, nbits);

	int run = 0;
	for (int i = 1; i < 64; i++) {
		int v = q[i];
		if (!v) { run++; continue; }
		while (run >= 16) {
			putBits(w, ac->code[0xF0], ac->size[0xF0]);
			run -= 16;
		}
		mag = v < 0 ? -v : v;
		nbits = 0;
		while (mag) { nbits++; mag >>= 1; }
		int sym = (run << 4) | nbits;
		putBits(w, ac->code[sym], ac->size[sym]);
		putBits(w, v < 0 ? v - 1 : v, nbits);
		run = 0;
	}
	if (run) putBits(w, ac->code[0x00], ac->size[0x00]);
}

static void scaleQuant(const uint8_t *base, int quality, uint8_t *out, float *divisors)
{
	static const float aan[8] = {
		1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
		1.0f, 0.785694958f, 0.541196100f, 0.275899379f
	};
	int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
	for (int i = 0; i < 64; i++) {
		int t = (base[i] * scale + 50) / 100;
		t = t < 1 ? 1 : (t > 255 ? 255 : t);
		out[i] = t;
		divisors[i] = 1.0f / (t * aan[i >> 3] * aan[i & 7] * 8.0f);
	}
}

size_t Enc_JPEG(const uint8_t *yuy2, int width, int height, int stride, int quality, uint8_t *out, size_t outCap)
{
	uint8_t lumaQ[64], chromaQ[64];
	float lumaDiv[64], chromaDiv[64];
	BitWriter w = { out, out + outCap, 0, 0, 0 };

	initTables();
	quality = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
	scaleQuant(stdLumaQuant, quality, lumaQ, lumaDiv);
	scaleQuant(stdChromaQuant, quality, chromaQ, chromaDiv);

	putMarker(&w, 0xD8, 0);		// SOI
	putMarker(&w, 0xE0, 16);	// APP0 JFIF
	static const uint8_t jfif[14] = {'J','F','I','F',0, 1,1, 0, 0,1, 0,1, 0,0};
	for (int i = 0; i < 14; i++) putByte(&w, jfif[i]);

	putMarker(&w, 0xDB, 2 + 2 * 65);	// DQT
	putByte(&w, 0);
	for (int i = 0; i < 64; i++) putByte(&w, lumaQ[zigzag[i]]);
	putByte(&w, 1);
	for (int i = 0; i < 64; i++) putByte(&w, chromaQ[zigzag[i]]);

	putMarker(&w, 0xC0, 17);	// SOF0, Y is 2x1 so chroma keeps the YUY2 sampling
	putByte(&w, 8);
	putByte(&w, height >> 8); putByte(&w, height);
	putByte(&w, width >> 8);  putByte(&w, width);
	putByte(&w, 3);
	putByte(&w, 1); putByte(&w, 0x21); putByte(&w, 0);
	putByte(&w, 2); putByte(&w, 0x11); putByte(&w, 1);
	putByte(&w, 3); putByte(&w, 0x11); putByte(&w, 1);

	putHuffTable(&w, 0x00, dcLumaBits, dcVals);
	putHuffTable(&w, 0x10, acLumaBits, acLumaVals);
	putHuffTable(&w, 0x01, dcChromaBits, dcVals);
	putHuffTable(&w, 0x11, acChromaBits, acChromaVals);

	putMarker(&w, 0xDA, 12);	// SOS
	putByte(&w, 3);
	putByte(&w, 1); putByte(&w, 0x00);
	putByte(&w, 2); putByte(&w, 0x11);
	putByte(&w, 3); putByte(&w, 0x11);
	putByte(&w, 0); putByte(&w, 63); putByte(&w, 0);

	int dcY = 0, dcCb = 0, dcCr = 0;
	int pairs = width / 2;
	float y0[64], y1[64], cb[64], cr[64];
	for (int my = 0; my < height; my += 8) {
		for (int mx = 0; mx < width; mx += 16) {
			for (int r = 0; r < 8; r++) {
				int row = my + r < height ? my + r : height - 1;
				const uint8_t *line = yuy2 + row * stride;
				for (int c = 0; c < 8; c++) {
					int p0 = mx + c, p1 = mx + 8 + c;
					if (p0 >= width) p0 = width - 1;
					if (p1 >= width) p1 = width - 1;
					y0[r*8+c] = fullRangeY[line[p0 * 2]] - 128.0f;
					y1[r*8+c] = fullRangeY[line[p1 * 2]] - 128.0f;
					int pair = mx / 2 + c;
					if (pair >= pairs) pair = pairs - 1;
					cb[r*8+c] = fullRangeC[line[pair * 4 + 1]] - 128.0f;
					cr[r*8+c] = fullRangeC[line[pair * 4 + 3]] - 128.0f;
				}
			}
			encodeBlock(&w, y0, lumaDiv, &dcY, &dcLuma, &acLuma);
			encodeBlock(&w, y1, lumaDiv, &dcY, &dcLuma, &acLuma);
			encodeBlock(&w, cb, chromaDiv, &dcCb, &dcChroma, &acChroma);
			encodeBlock(&w, cr, chromaDiv, &dcCr, &dcChroma, &acChroma);
			if (w.overflow) return 0;
		}
	}
	if (w.bits) putBits(&w, 0x7F, 8 - w.bits);	// pad with ones
	putMarker(&w, 0xD9, 0);	// EOI
	return w.overflow ? 0 : (size_t)(w.p - out);
}

/* ---------------------------------------------------------------------------
 * PNG
 * ------------------------------------------------------------------------- */

static void be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static uint8_t *pngChunk(uint8_t *p, const char *type, const uint8_t *data, uint32_t len)
{
	be32(p, len);
	memcpy(p + 4, type, 4);
	if (len && data != p + 8) memmove(p + 8, data, len);
	be32(p + 8 + len, libdeflate_crc32(0, p + 4, len + 4));
	return p + 12 + len;
}

static void yuvToRGB(int y, int u, int v, uint8_t *rgb)
{
	int c = 298 * (y - 16) + 128, d = u - 128, e = v - 128;
	rgb[0] = clamp255((c + 409 * e) >> 8);
	rgb[1] = clamp255((c - 100 * d - 208 * e) >> 8);
	rgb[2] = clamp255((c + 516 * d) >> 8);
}

size_t Enc_PNG(const uint8_t *yuy2, int width, int height, int stride, int lineRepeat, uint8_t **out)
{
	*out = NULL;
	if (lineRepeat < 1) lineRepeat = 1;
	int outH = height * lineRepeat;
	size_t rowBytes = 1 + (size_t)width * 3;
	size_t rawSize = rowBytes * outH;
	uint8_t *raw = malloc(rawSize);
	if (!raw) return 0;

	for (int y = 0; y < height; y++) {
		const uint8_t *s = yuy2 + y * stride;
		uint8_t *row = raw + (size_t)y * lineRepeat * rowBytes;
		uint8_t *d = row + 1;
		row[0] = 1;	// Sub filter
		for (int x = 0; x + 1 < width; x += 2, s += 4, d += 6) {
			yuvToRGB(s[0], s[1], s[3], d);
			yuvToRGB(s[2], s[1], s[3], d + 3);
		}
		for (int i = rowBytes - 1; i > 3; i--) row[i] -= row[i - 3];
		for (int r = 1; r < lineRepeat; r++) memcpy(row + r * rowBytes, row, rowBytes);
	}

	struct libdeflate_compressor *c = libdeflate_alloc_compressor(6);
	if (!c) {
		free(raw);
		return 0;
	}
	size_t bound = libdeflate_zlib_compress_bound(c, rawSize);
	uint8_t *png = malloc(8 + 25 + 12 + bound + 12);
	if (!png) {
		libdeflate_free_compressor(c);
		free(raw);
		return 0;
	}
	static const uint8_t sig[8] = {0x89,'P','N','G','\r','\n',0x1A,'\n'};
	memcpy(png, sig, 8);
	uint8_t ihdr[13];
	be32(ihdr, width);
	be32(ihdr + 4, outH);
	ihdr[8] = 8;	// bit depth
	ihdr[9] = 2;	// truecolour
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	uint8_t *p = pngChunk(png + 8, "IHDR", ihdr, 13);
	size_t zlen = libdeflate_zlib_compress(c, raw, rawSize, p + 8, bound);
	libdeflate_free_compressor(c);
	free(raw);
	if (!zlen) {
		free(png);
		return 0;
	}
	p = pngChunk(p, "IDAT", p + 8, zlen);
	p = pngChunk(p, "IEND", NULL, 0);
	*out = png;
	return p - png;
}

/* ---------------------------------------------------------------------------
 * AVI
 * ------------------------------------------------------------------------- */

static uint8_t *le32(uint8_t *p, uint32_t v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
	return p + 4;
}

static uint8_t *le16(uint8_t *p, uint16_t v)
{
	p[0] = v; p[1] = v >> 8;
	return p + 2;
}

static uint8_t *fourcc(uint8_t *p, const char *cc)
{
	memcpy(p, cc, 4);
	return p + 4;
}

void Avi_Header(uint8_t out[AVI_HEADER_SIZE], const AviInfo *info)
{
	uint8_t *p = out;
	uint32_t usPerFrame = (uint32_t)((uint64_t)1000000 * info->scale / info->rate);
	uint32_t bytesPerSec = (uint32_t)((uint64_t)info->maxChunk * info->rate / info->scale);

	p = fourcc(p, "RIFF");
	p = le32(p, AVI_HEADER_SIZE - 8 + info->moviSize + (info->idxSize ? 8 + info->idxSize : 0));
	p = fourcc(p, "AVI ");

	p = fourcc(p, "LIST");
	p = le32(p, 192);
	p = fourcc(p, "hdrl");

	p = fourcc(p, "avih");
	p = le32(p, 56);
	p = le32(p, usPerFrame);
	p = le32(p, bytesPerSec);
	p = le32(p, 0);				// padding granularity
	p = le32(p, info->idxSize ? 0x10 : 0);	// AVIF_HASINDEX
	p = le32(p, info->frames);
	p = le32(p, 0);				// initial frames
	p = le32(p, 1);				// streams
	p = le32(p, info->maxChunk);
	p = le32(p, info->width);
	p = le32(p, info->height);
	for (int i = 0; i < 4; i++) p = le32(p, 0);

	p = fourcc(p, "LIST");
	p = le32(p, 116);
	p = fourcc(p, "strl");

	p = fourcc(p, "strh");
	p = le32(p, 56);
	p = fourcc(p, "vids");
	p = fourcc(p, "MJPG");
	p = le32(p, 0);				// flags
	p = le16(p, 0);				// priority
	p = le16(p, 0);				// language
	p = le32(p, 0);				// initial frames
	p = le32(p, info->scale);
	p = le32(p, info->rate);
	p = le32(p, 0);				// start
	p = le32(p, info->frames);
	p = le32(p, info->maxChunk);
	p = le32(p, 0xFFFFFFFF);	// quality
	p = le32(p, 0);				// sample size
	p = le16(p, 0);
	p = le16(p, 0);
	p = le16(p, info->width);
	p = le16(p, info->height);

	p = fourcc(p, "strf");
	p = le32(p, 40);
	p = le32(p, 40);
	p = le32(p, info->width);
	p = le32(p, info->height);
	p = le16(p, 1);				// planes
	p = le16(p, 24);			// bit count
	p = fourcc(p, "MJPG");
	p = le32(p, info->width * info->height * 3);
	for (int i = 0; i < 4; i++) p = le32(p, 0);

	p = fourcc(p, "LIST");
	p = le32(p, 4 + info->moviSize);
	p = fourcc(p, "movi");
}

uint32_t Avi_ChunkHeader(uint8_t out[8], uint32_t size)
{
	fourcc(out, "00dc");
	le32(out + 4, size);
	return 8 + ((size + 1) & ~1);
}

void Avi_IndexEntry(uint8_t out[16], uint32_t moviOffset, uint32_t size)
{
	uint8_t *p = fourcc(out, "00dc");
	p = le32(p, size ? 0x10 : 0);	// every JPEG is a keyframe, empty chunks repeat the last one
	p = le32(p, moviOffset);
	le32(p, size);
}

void Avi_IndexHeader(uint8_t out[8], uint32_t entries)
{
	fourcc(out, "idx1");
	le32(out + 4, entries * 16);
}
