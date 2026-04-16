#define _GNU_SOURCE
#include "display.h"
#include "config.h"
#include "font.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

extern void drm_kick(DisplayDev *d);
extern void fbdev_kick(DisplayDev *d);
extern bool fbdev_init(DisplayDev *d);

static uint32_t palette[256];

/* Pre-computed framebuffer stride in uint32_t units */
static uint32_t g_stride;
static uint32_t *g_fb;
static int g_fb_w, g_fb_h;

static inline __attribute__((always_inline)) uint32_t blend_px(uint32_t bg,
                                                               uint32_t fg,
                                                               uint32_t a) {
  if (__builtin_expect(a == 0, 0))
    return bg;
  if (__builtin_expect(a == 255, 0))
    return fg;
  uint32_t inv = 255 - a;
  uint32_t rb = ((fg & 0xFF00FF) * a + (bg & 0xFF00FF) * inv) >> 8;
  uint32_t g = ((fg & 0x00FF00) * a + (bg & 0x00FF00) * inv) >> 8;
  return 0xFF000000u | (rb & 0xFF00FF) | (g & 0x00FF00);
}

/* Map logical (x,y) to physical framebuffer index.
 * Compile-time constant ROTATION lets the compiler dead-code-eliminate. */
static inline __attribute__((always_inline)) uint32_t *fb_px(int x, int y) {
#if ROTATION == 0
  return g_fb + (uint32_t)y * g_stride + (uint32_t)x;
#elif ROTATION == 1
  return g_fb + (uint32_t)x * g_stride + (uint32_t)(g_fb_w - 1 - y);
#elif ROTATION == 2
  return g_fb + (uint32_t)(g_fb_h - 1 - y) * g_stride +
         (uint32_t)(g_fb_w - 1 - x);
#elif ROTATION == 3
  return g_fb + (uint32_t)(g_fb_h - 1 - x) * g_stride + (uint32_t)y;
#else
  /* Runtime fallback */
  int rx = x, ry = y;
  if (ROTATION == 1) {
    rx = g_fb_w - 1 - y;
    ry = x;
  } else if (ROTATION == 2) {
    rx = g_fb_w - 1 - x;
    ry = g_fb_h - 1 - y;
  } else if (ROTATION == 3) {
    rx = y;
    ry = g_fb_h - 1 - x;
  }
  return g_fb + (uint32_t)ry * g_stride + (uint32_t)rx;
#endif
}

/* Horizontal fill: for rotation 0, pixels are contiguous in memory. */
static inline __attribute__((always_inline)) void hfill(int x, int y, int len,
                                                        uint32_t col) {
#if ROTATION == 0
  uint32_t *p = g_fb + (uint32_t)y * g_stride + (uint32_t)x;
  /* gcc auto-vectorizes this for NEON on aarch64 */
  for (int i = 0; i < len; i++)
    p[i] = col;
#else
  for (int i = 0; i < len; i++)
    *fb_px(x + i, y) = col;
#endif
}

/* Vertical fill: for rotation 0, stride-step. */
static inline __attribute__((always_inline)) void vfill(int x, int y, int len,
                                                        uint32_t col) {
#if ROTATION == 0
  uint32_t *p = g_fb + (uint32_t)y * g_stride + (uint32_t)x;
  for (int i = 0; i < len; i++) {
    *p = col;
    p += g_stride;
  }
#else
  for (int i = 0; i < len; i++)
    *fb_px(x, y + i) = col;
#endif
}

static inline __attribute__((always_inline)) uint32_t
resolve_color(uint32_t c) {
  if (__builtin_expect(!IS_RGB(c), 1))
    return palette[c & 0xFF];
  uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
#if COLOR_BGR
  return 0xFF000000u | r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
#else
  return 0xFF000000u | b | ((uint32_t)g << 8) | ((uint32_t)r << 16);
#endif
}

/* Braille dot bit layout */
static const int braille_map[2][4] = {{0, 1, 2, 6}, {3, 4, 5, 7}};

void display_render(DisplayDev *d, Term *t) {
  if (!g_fb)
    return;
  bool any = t->screen_dirty;
  t->screen_dirty = false;
  const int bl = font_baseline();
  const int cw = d->cell_w, ch = d->cell_h;
  const int cols = t->cols, vr = t->view_row;

  for (int r = 0; r < t->rows; r++) {
    if (!t->dirty[r] && !any)
      continue;
    t->dirty[r] = false;

    const Cell *row_base = &t->cells[(vr + r) * cols];
    const int y0_base = r * ch + MARGIN_TOP;

    for (int c = 0; c < cols; c++) {
      const Cell *cl = &row_base[c];
      if (__builtin_expect(cl->width == 0, 0))
        continue;

      const int gc = (cl->width >= 2) ? 2 : 1;
      const int pw = cw * gc;
      const int x0 = c * cw + MARGIN_LEFT;
      const int y0 = y0_base;

      uint32_t fg = resolve_color(cl->fg);
      uint32_t bg = resolve_color(cl->bg);

      if (__builtin_expect(cl->attr & ATTR_REVERSE, 0)) {
        uint32_t t2 = fg;
        fg = bg;
        bg = t2;
      }
      if (__builtin_expect((cl->attr & ATTR_BOLD) && !IS_RGB(cl->fg) &&
                               (cl->fg & 0xFF) < 8,
                           0))
        fg = palette[(cl->fg & 0xFF) + 8];
      if (__builtin_expect((cl->attr & ATTR_DIM) && !(cl->attr & ATTR_REVERSE),
                           0))
        fg = blend_px(bg, fg, 128);

      /* Background fill */
      for (int y = 0; y < ch; y++)
        hfill(x0, y0 + y, pw, bg);

      const uint32_t code = cl->code;
      if (__builtin_expect(code <= ' ' || code == 0xFEFF, 0))
        goto cell_done;

      /* Braille U+2800-U+28FF */
      if (__builtin_expect(code >= 0x2800 && code <= 0x28FF, 0)) {
        const uint8_t m = (uint8_t)(code - 0x2800);
        if (m == 0)
          goto cell_done; /* empty braille = blank */
        const int dw = pw / 2, dh = ch / 4;
        const int dotw = dw / 2 > 0 ? dw / 2 : 1;
        const int doth = dh / 2 > 0 ? dh / 2 : 1;
        for (int dr = 0; dr < 4; dr++)
          for (int dc = 0; dc < 2; dc++) {
            if (!((m >> braille_map[dc][dr]) & 1))
              continue;
            int dx = x0 + dc * dw + dw / 4;
            int dy = y0 + dr * dh + dh / 4;
            for (int yy = 0; yy < doth; yy++)
              hfill(dx, dy + yy, dotw, fg);
          }
        goto cell_done;
      }

      /* Box-drawing U+2500-U+257F */
      if (__builtin_expect(code >= 0x2500 && code <= 0x257F, 0)) {
        int mx = pw / 2, my = ch / 2;
        uint8_t flags = 0; /* bits: 0=left 1=right 2=up 3=down */
        if (code <= 0x2501)
          flags = 3;
        else if (code <= 0x2503)
          flags = 12;
        else if (code <= 0x250F)
          flags = (uint8_t)((code - 0x2504 <= 3)
                                ? 3
                                : (((code - 0x250C) < 4) ? 10 : 3));
        else {
          /* Lookup table for 0x250C-0x254B: l,r,u,d encoded in 4 bits */
          static const uint8_t box_flags[] = {
              10, 10, 10, 10, 9,  9,  9,  9,
              6,  6,  6,  6,  5,  5,  5,  5,  /* 250C-251B */
              14, 14, 14, 14, 14, 14, 14, 14, /* 251C-2523 */
              13, 13, 13, 13, 13, 13, 13, 13, /* 2524-252B */
              11, 11, 11, 11, 11, 11, 11, 11, /* 252C-2533 */
              7,  7,  7,  7,  7,  7,  7,  7,  /* 2534-253B */
              15, 15, 15, 15, 15, 15, 15, 15,
              15, 15, 15, 15, 15, 15, 15, 15 /* 253C-254B */
          };
          int idx = (int)(code - 0x250C);
          if (idx >= 0 && idx < (int)sizeof(box_flags))
            flags = box_flags[idx];
          else
            flags = 3; /* default: horizontal */
        }
        if (flags & 1)
          hfill(x0, y0 + my, mx + 1, fg);
        if (flags & 2)
          hfill(x0 + mx, y0 + my, pw - mx, fg);
        if (flags & 4)
          vfill(x0 + mx, y0, my + 1, fg);
        if (flags & 8)
          vfill(x0 + mx, y0 + my, ch - my, fg);
        goto cell_done;
      }

      /* Block elements U+2580-U+2588 */
      if (__builtin_expect(code >= 0x2580 && code <= 0x2588, 0)) {
        int fh, ys;
        if (code == 0x2588) {
          fh = ch;
          ys = 0;
        } else if (code == 0x2580) {
          fh = ch / 2;
          ys = 0;
        } else {
          fh = ch * (int)(code - 0x2580) / 8;
          ys = ch - fh;
        }
        for (int y = ys; y < ys + fh; y++)
          hfill(x0, y0 + y, pw, fg);
        goto cell_done;
      }

      /* FreeType glyph */
      {
        const Glyph *g = font_glyph(code);
        if (__builtin_expect(g->px != NULL, 1)) {
          const int gx = x0 + g->bx;
          const int gy = y0 + bl - g->by;
          /* Clamp glyph to cell bounds */
          int y_start = gy < y0 ? y0 - gy : 0;
          int y_end = (gy + g->bh > y0 + ch) ? y0 + ch - gy : g->bh;
          int x_start = gx < x0 ? x0 - gx : 0;
          int x_end = (gx + g->bw > x0 + pw) ? x0 + pw - gx : g->bw;
          for (int gy2 = y_start; gy2 < y_end; gy2++) {
            const uint8_t *src = g->px + gy2 * g->bw;
            int py = gy + gy2;
            for (int gx2 = x_start; gx2 < x_end; gx2++) {
              uint8_t a = src[gx2];
              if (__builtin_expect(a != 0, 1))
                *fb_px(gx + gx2, py) = (a == 255) ? fg : blend_px(bg, fg, a);
            }
          }
        }
      }

    cell_done:
      if (__builtin_expect(cl->attr & ATTR_UNDERLINE, 0))
        hfill(x0, y0 + ch - 2, pw, fg);
    }
    any = true;
  }

  /* Cursor */
  if (t->cursor_visible && t->cy < t->rows) {
    int x0 = t->cx * cw + MARGIN_LEFT;
    int y0 = t->cy * ch + MARGIN_TOP;
    uint32_t clr = palette[CURSOR_COLOR];
    hfill(x0, y0 + ch - 2, cw, clr);
    hfill(x0, y0 + ch - 1, cw, clr);
  }

  if (any)
    display_kick(d);
}

void display_blank(DisplayDev *d, bool blank) {
  (void)d;
  (void)blank;
}

void display_kick(DisplayDev *d) {
  if (d->is_drm)
    drm_kick(d);
  else
    fbdev_kick(d);
}

void display_free(DisplayDev *d) {
  if (d->buf.map)
    munmap(d->buf.map, d->buf.size);
  if (d->fd >= 0)
    close(d->fd);
  d->fd = -1;
  d->buf.map = NULL;
  g_fb = NULL;
  font_free();
}

static void ensure_node(const char *path, int maj, int min) {
  struct stat st;
  if (stat(path, &st) == 0 && S_ISCHR(st.st_mode) &&
      major(st.st_rdev) == (unsigned)maj && minor(st.st_rdev) == (unsigned)min)
    return;
  (void)mknod(path, S_IFCHR | 0666, makedev((unsigned)maj, (unsigned)min));
}

bool display_init(DisplayDev *d) {
  FILE *f = fopen(BACKLIGHT_PATH, "w");
  if (f) {
    fprintf(f, "%d\n", BACKLIGHT_VAL);
    fclose(f);
  }

  if (!font_init(&d->cell_w, &d->cell_h)) {
    LOG("font_init failed");
    return false;
  }

  ensure_node(DRM_DEVICE, DRM_MAJOR, DRM_MINOR);
  if (drm_init_dev(d)) {
    d->is_drm = true;
  } else {
    ensure_node(FB_DEVICE, FB_MAJOR, FB_MINOR);
    if (!fbdev_init(d))
      return false;
    d->is_drm = false;
  }

  /* Cache framebuffer globals for hot path */
  g_stride = d->buf.pitch / 4;
  g_fb = d->buf.map;
  g_fb_w = d->width;
  g_fb_h = d->height;

  /* Build 256-color palette */
  for (int i = 0; i < 256; i++) {
    uint8_t r = 0, g = 0, b = 0;
    if (i < 16) {
      r = (i & 1) ? 170 : 0;
      g = (i & 2) ? 170 : 0;
      b = (i & 4) ? 170 : 0;
      if (i == 8) {
        r = g = b = 85;
      } else if (i > 8) {
        if (i & 1)
          r = 255;
        if (i & 2)
          g = 255;
        if (i & 4)
          b = 255;
      }
      if (i == 7)
        r = g = b = 192;
    } else if (i < 232) {
      int idx = i - 16;
      int ri = idx / 36, gi = (idx / 6) % 6, bi = idx % 6;
      r = ri ? (uint8_t)(ri * 40 + 55) : 0;
      g = gi ? (uint8_t)(gi * 40 + 55) : 0;
      b = bi ? (uint8_t)(bi * 40 + 55) : 0;
    } else {
      r = g = b = (uint8_t)((i - 232) * 10 + 8);
    }
#if COLOR_BGR
    palette[i] = 0xFF000000u | r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
#else
    palette[i] = 0xFF000000u | b | ((uint32_t)g << 8) | ((uint32_t)r << 16);
#endif
  }
  return true;
}
