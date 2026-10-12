/* -----------------------------------------------------------
      capture_enc.h - Image and video encoders for screen capture

      Everything here works on YUY2 frames as the VI scans them
      out of the XFB (bytes Y0 U Y1 V, BT.601 limited range) and
      has no libogc dependency, so it can be tested on a PC.
   ----------------------------------------------------------- */

#ifndef CAPTURE_ENC_H
#define CAPTURE_ENC_H

#include <stddef.h>
#include <stdint.h>

// Box filters a YUY2 frame to half width, and half height too when halveV is set.
// The output is packed: its stride is (width / 2) * 2 bytes.
void Enc_HalveYUY2(const uint8_t *src, int width, int height, int stride, int halveV, uint8_t *dst);

// Baseline JPEG (4:2:2, full range JFIF) straight from YUY2. Returns the size, or 0 if out is too small.
size_t Enc_JPEG(const uint8_t *yuy2, int width, int height, int stride, int quality, uint8_t *out, size_t outCap);

// 24-bit PNG from YUY2, each source line repeated lineRepeat times. Returns a malloc'd file in *out.
size_t Enc_PNG(const uint8_t *yuy2, int width, int height, int stride, int lineRepeat, uint8_t **out);

// AVI (MJPEG, RIFF 1.0) building blocks
#define AVI_HEADER_SIZE   224	// everything before the first movi chunk
#define AVI_MOVI_OFFSET   220	// idx1 offsets are relative to the 'movi' fourcc

typedef struct {
	int width, height;
	uint32_t rate, scale;		// frames per second = rate / scale
	uint32_t frames;
	uint32_t moviSize;			// bytes of chunks written after the header
	uint32_t maxChunk;
	uint32_t idxSize;			// bytes of idx1 entries following the movi list
} AviInfo;

void Avi_Header(uint8_t out[AVI_HEADER_SIZE], const AviInfo *info);
// Fills an 8 byte '00dc' chunk header, returns the padded chunk size including the header
uint32_t Avi_ChunkHeader(uint8_t out[8], uint32_t size);
// Fills a 16 byte idx1 entry
void Avi_IndexEntry(uint8_t out[16], uint32_t moviOffset, uint32_t size);
// Fills the 8 byte idx1 chunk header
void Avi_IndexHeader(uint8_t out[8], uint32_t entries);

#endif
