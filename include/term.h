#ifndef TERM_H
#define TERM_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

/* Cell attribute flags */
#define ATTR_BOLD (1u << 0)
#define ATTR_REVERSE (1u << 1)
#define ATTR_UNDERLINE (1u << 2)
#define ATTR_DIM (1u << 3)
#define ATTR_BLINK (1u << 4)
#define ATTR_ITALIC (1u << 5)

/* Color encoding: bit31 set = 24-bit RGB; clear = palette index 0-255 */
#define COLOR_RGB_FLAG 0x80000000u
#define MKRGB(r, g, b)                                                         \
  (COLOR_RGB_FLAG | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) |             \
   (uint32_t)(b))
#define IS_RGB(c) ((c) & COLOR_RGB_FLAG)

typedef struct {
  uint32_t code;   /* Unicode codepoint */
  uint32_t fg, bg; /* Color: palette index or MKRGB() value */
  uint8_t attr;    /* ATTR_* bitmask */
  uint8_t width;   /* 1=normal, 2=double-wide, 0=continuation of wide */
} Cell;

typedef enum {
  ST_GROUND,
  ST_ESC,
  ST_CSI,
  ST_OSC,
  ST_ESC_INTERMEDIATE
} EscState;

typedef struct Term {
  int cols, rows;
  int cx, cy;
  int scroll_top, scroll_bot;
  int cell_w, cell_h;
  uint32_t fg, bg;
  uint8_t attr;
  bool cursor_visible;
  int saved_cx, saved_cy;
  uint32_t saved_fg, saved_bg;
  uint8_t saved_attr;

  Cell *cells, *cells_alt;
  bool *dirty;
  int view_row, total_rows;
  bool use_alt_screen;
  bool charset_gfx;
  bool screen_dirty;

  /* Escape sequence parser */
  EscState state;
  char escbuf[ESC_BUF_MAX];
  int esclen;
  int params[CSI_PARAMS_MAX];
  int nparams;
  bool priv;

  /* UTF-8 decoder */
  uint32_t utf8_code;
  int utf8_expect;
} Term;

void term_init(Term *t, int px_w, int px_h, int cell_w, int cell_h);
void term_free(Term *t);
void term_write(Term *t, const uint8_t *buf, int n);
void term_scroll(Term *t, int delta);
void term_snap_to_bottom(Term *t);

#endif
