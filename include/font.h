#ifndef FONT_H
#define FONT_H

#include <stdbool.h>
#include <stdint.h>

/* Pre-rasterized glyph from FreeType */
typedef struct {
  uint32_t cp;
  int bx, by;  /* bitmap_left, bitmap_top */
  int bw, bh;  /* bitmap dimensions */
  int adv;     /* advance in pixels */
  uint8_t *px; /* alpha bitmap (bw*bh), NULL for empty glyphs */
} Glyph;

/* Load font from paths defined in config.h; writes cell size to *cw, *ch */
bool font_init(int *cw, int *ch);
void font_free(void);

/* Get glyph (cached). Never returns NULL. */
const Glyph *font_glyph(uint32_t cp);

/* Returns 2 for fullwidth CJK/emoji, 1 otherwise */
int font_wcwidth(uint32_t cp);

/* Ascender in pixels (baseline offset from cell top) */
int font_baseline(void);

#endif
