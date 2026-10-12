/* -----------------------------------------------------------
      pngdec.h - minimal PNG decoder on top of zlib

      Handles the non-interlaced 8 bit images that cover art
      sites (GameTDB) serve: greyscale, RGB, palette, grey+alpha
      and RGBA. Output is always 8 bit RGBA, row major.
   ----------------------------------------------------------- */

#ifndef PNGDEC_H
#define PNGDEC_H

#include <gctypes.h>

// Returns a malloc'ed RGBA buffer of *w x *h pixels, or NULL
u8 *PNG_Decode(const u8 *data, u32 size, int *w, int *h);

#endif
