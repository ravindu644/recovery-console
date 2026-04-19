#define _GNU_SOURCE
#include "display.h"
#include "config.h"
#include "font.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* Force backlight/brightness to the level defined in config.h */
static void backlight_wake(void) {
  FILE *bf = fopen(BACKLIGHT_PATH, "w");
  if (bf) {
    fprintf(bf, "%d\n", BACKLIGHT_VAL);
    fclose(bf);
  }
}

/*  VT state  */
static int g_vt_fd = -1;          /* our active VT fd              */
static struct vt_mode g_vt_saved; /* original VT mode to restore   */
static bool g_vt_restored = false;
volatile sig_atomic_t g_vt_active = 1; /* 1 = we own display            */

int vt_get_fd(void) { return g_vt_fd; }

/*  VT init  *
 *
 *  Opens the currently active VT, sets KD_GRAPHICS (suppresses kernel text
 *  console painting for BOTH DRM and fbdev), then registers VT_PROCESS mode
 *  so the kernel asks permission before switching VTs.
 *
 *  The signal assignments (SIGUSR1 = release, SIGUSR2 = acquire) must have
 *  their handlers installed by the caller BEFORE vt_init() is called.
 */
int vt_init(DisplayDev *d) {
  (void)d; /* master is grabbed/dropped per render in drm_kick(); not needed
              here */
  /* /dev/tty0 always refers to the currently active VT for ioctls. */
  int tty0 = open("/dev/tty0", O_RDWR | O_CLOEXEC);
  if (tty0 < 0)
    tty0 = open("/dev/console", O_RDWR | O_CLOEXEC);

  int target_vt = -1;
  if (tty0 >= 0) {
    /* Step 1: Query for the first available free VT. */
    if (ioctl(tty0, VT_OPENQRY, &target_vt) < 0 || target_vt <= 0) {
      /* Step 2: Fallback – use the currently active VT. */
      struct vt_stat vts = {0};
      if (ioctl(tty0, VT_GETSTATE, &vts) == 0 && vts.v_active)
        target_vt = vts.v_active;
    }
    close(tty0);
  }
  if (target_vt <= 0)
    target_vt = 1;

  char vtpath[32];
  snprintf(vtpath, sizeof(vtpath), "/dev/tty%d", target_vt);
  LOG("VT auto-picked: %s", vtpath);
  g_vt_fd = open(vtpath, O_RDWR | O_CLOEXEC);
  if (g_vt_fd < 0) {
    /* Fallback: try controlling tty of this process. */
    g_vt_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
  }
  if (g_vt_fd < 0) {
    /* No VT available (headless / Android init context).
     * Render will proceed without VT switching support. */
    g_vt_active = 1;
    return -1;
  }

  /* Save original mode so we can restore it cleanly on exit. */
  if (ioctl(g_vt_fd, VT_GETMODE, &g_vt_saved) < 0) {
    close(g_vt_fd);
    g_vt_fd = -1;
    g_vt_active = 1;
    return -1;
  }
  g_vt_restored = false;

  /* Switch to graphics mode – stops fbcon painting text over our pixels.
   * Required for BOTH DRM and fbdev backends.                         */
  ioctl(g_vt_fd, KDSETMODE, KD_GRAPHICS);

  /* Register VT_PROCESS: kernel sends SIGUSR1 before switching away,
   * SIGUSR2 when switching back. Caller has set up signal handlers.   */
  struct vt_mode vm = {
      .mode = VT_PROCESS,
      .waitv = 0,
      .relsig = SIGUSR1, /* kernel: "please release the VT"   */
      .acqsig = SIGUSR2, /* kernel: "VT is yours again"       */
      .frsig = 0,
  };
  if (ioctl(g_vt_fd, VT_SETMODE, &vm) < 0) {
    /* VT_PROCESS unavailable (old Android kernel?) – fall back gracefully. */
    ioctl(g_vt_fd, KDSETMODE, KD_TEXT);
    close(g_vt_fd);
    g_vt_fd = -1;
    g_vt_active = 1;
    return -1;
  }

  /* We use a transient master model: drm_kick() grabs/drops master per frame.
   * This allows Xorg/Wayland on other VTs to work without "device busy". */
  g_vt_active = 1;
  return g_vt_fd;
}

/*  VT restore  *
 *  Called from display_free() to put the VT back into text / auto mode.
 *  Idempotent – safe to call multiple times.                            */
void vt_restore(void) {
  if (g_vt_fd < 0 || g_vt_restored)
    return;
  g_vt_restored = true;
  ioctl(g_vt_fd, KDSETMODE, KD_TEXT);
  ioctl(g_vt_fd, VT_SETMODE, &g_vt_saved);
  close(g_vt_fd);
  g_vt_fd = -1;
}

/*  VT release (deferred from SIGUSR1)  *
 *
 *  Called from the main select() loop when g_vt_rel flag is set.
 *
 *  For DRM:   drop master so X/Wayland can take the device.
 *  For fbdev: no device token to drop; just stop rendering (g_vt_active=0).
 *
 *  Must ack via VT_RELDISP(1) or the kernel will not switch.
 */
void vt_release(DisplayDev *d) {
  g_vt_active = 0;
  if (d->is_drm)
    drm_drop_master(d);
  if (g_vt_fd >= 0)
    ioctl(g_vt_fd, VT_RELDISP, 1); /* ack: kernel may now switch */
}

/*  VT acquire (deferred from SIGUSR2)  *
 *
 *  Called from the main select() loop when g_vt_acq flag is set.
 *
 *  1. Ack acquisition (polite; not strictly required on Linux).
 *  2. For DRM: re-grab master and re-program CRTC with our FB.
 *  3. Full framebuffer clear – eliminates "layers behind margins"
 *     artefacts from the previous owner (X/TWRP/etc.).
 *  4. Set g_vt_active=1 so the render loop resumes.
 *
 *  Caller must mark all term rows dirty and call display_render().
 */
void vt_acquire(DisplayDev *d) {
  if (g_vt_fd >= 0)
    ioctl(g_vt_fd, VT_RELDISP, VT_ACKACQ);

  if (d->is_drm) {
    /* Grab master and re-attach FB. We keep master until vt_release(). */
    drm_set_master(d);
    drm_reprogram_crtc(d);
    drm_kickstart(d); /* Atomic power-on cycle (Blank -> Unblank) */
  } else {
    backlight_wake();
  }

  /* Full clear render buffer; VT owner may have dirtied scanout. */
#if USE_SHADOW_BUFFER
  if (d->shadow && d->buf.size > 0)
    memset(d->shadow, 0, d->buf.size);
#else
  if (d->buf.map && d->buf.size > 0)
    memset(d->buf.map, 0, d->buf.size);
#endif

  g_vt_active = 1;
}

/*  Framebuffer cache (hot path)  */
static uint32_t g_stride; /* pitch in uint32_t units */
static uint32_t *g_fb;
static int g_fb_w, g_fb_h;

/*  256-color palette  */
static uint32_t palette[256];

/*  Alpha blend – branch-predicted for opaque/transparent fast paths  */
static inline __attribute__((always_inline)) uint32_t blend_px(uint32_t bg,
                                                               uint32_t fg,
                                                               uint32_t a) {
  if (__builtin_expect(a == 0, 0))
    return bg;
  if (__builtin_expect(a == 255, 0))
    return fg;
  uint32_t inv = 255 - a;
  uint32_t rb = ((fg & 0xFF00FFu) * a + (bg & 0xFF00FFu) * inv) >> 8;
  uint32_t g2 = ((fg & 0x00FF00u) * a + (bg & 0x00FF00u) * inv) >> 8;
  return 0xFF000000u | (rb & 0xFF00FFu) | (g2 & 0x00FF00u);
}

/*  Logical (x,y) -> physical framebuffer pointer  *
 * ROTATION is a compile-time constant; the compiler eliminates dead branches.*/
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

/* Horizontal fill – auto-vectorised by GCC/Clang (NEON / SSE). */
static inline __attribute__((always_inline)) void hfill(int x, int y, int len,
                                                        uint32_t col) {
#if ROTATION == 0
  uint32_t *p = g_fb + (uint32_t)y * g_stride + (uint32_t)x;
  for (int i = 0; i < len; i++)
    p[i] = col;
#else
  for (int i = 0; i < len; i++)
    *fb_px(x + i, y) = col;
#endif
}

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
  uint8_t r = (c >> 16) & 0xFF, g2 = (c >> 8) & 0xFF, b = c & 0xFF;
#if COLOR_BGR
  return 0xFF000000u | r | ((uint32_t)g2 << 8) | ((uint32_t)b << 16);
#else
  return 0xFF000000u | b | ((uint32_t)g2 << 8) | ((uint32_t)r << 16);
#endif
}

/* Braille dot bit layout (Unicode 2800-28FF). */
static const int braille_map[2][4] = {{0, 1, 2, 6}, {3, 4, 5, 7}};

/*  display_render  *
 *  Hot path. Called every time there is new PTY output, on VT acquire, or
 *  after a scroll. Skips unchanged rows. Guard on g_vt_active.         */
void display_render(DisplayDev *d, Term *t) {
  if (!g_fb || !g_vt_active)
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
        uint32_t tmp = fg;
        fg = bg;
        bg = tmp;
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

      /*  Braille U+2800-U+28FF  */
      if (__builtin_expect(code >= 0x2800 && code <= 0x28FF, 0)) {
        const uint8_t m = (uint8_t)(code - 0x2800);
        if (!m)
          goto cell_done;
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

      /*  Box-drawing U+2500-U+257F  */
      if (__builtin_expect(code >= 0x2500 && code <= 0x257F, 0)) {
        int mx = pw / 2, my = ch / 2;
        uint8_t flags = 0; /* bits: 0=left 1=right 2=up 3=down */
        bool dbl = (code >= 0x2550 && code <= 0x256C);
        bool rnd = (code >= 0x256D && code <= 0x2570);

        if (code <= 0x2501)
          flags = 3;
        else if (code <= 0x2503)
          flags = 12;
        else if (dbl) {
          static const uint8_t dbl_flags[] = {
              3,  12, 6,  6,  6,  5,  5,  5,  10, 10, 10, 9,  9,  9, 14,
              14, 14, 13, 13, 13, 11, 11, 11, 7,  7,  7,  15, 15, 15};
          int idx = (int)(code - 0x2550);
          flags =
              (idx >= 0 && idx < (int)sizeof(dbl_flags)) ? dbl_flags[idx] : 0;
        } else if (rnd) {
          static const uint8_t rnd_flags[] = {10, 9, 5, 6};
          flags = rnd_flags[code - 0x256D];
        } else {
          static const uint8_t box_flags[] = {
              10, 10, 10, 10, 9,  9,  9,  9,  6,  6,  6,  6,  5,  5,  5,  5,
              14, 14, 14, 14, 14, 14, 14, 14, 13, 13, 13, 13, 13, 13, 13, 13,
              11, 11, 11, 11, 11, 11, 11, 11, 7,  7,  7,  7,  7,  7,  7,  7,
              15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15};
          int idx = (int)(code - 0x250C);
          flags =
              (idx >= 0 && idx < (int)sizeof(box_flags)) ? box_flags[idx] : 3;
        }

        if (dbl) {
          /* Double lines: draw two parallel lines offset from center */
          int o = (ch > 16) ? 2 : 1;
          if (flags & 1) {
            hfill(x0, y0 + my - o, mx + 1 - o, fg);
            hfill(x0, y0 + my + o, mx + 1 + o, fg);
          }
          if (flags & 2) {
            hfill(x0 + mx + o, y0 + my - o, pw - mx - o, fg);
            hfill(x0 + mx - o, y0 + my + o, pw - mx + o, fg);
          }
          if (flags & 4) {
            vfill(x0 + mx - o, y0, my + 1 - o, fg);
            vfill(x0 + mx + o, y0, my + 1 + o, fg);
          }
          if (flags & 8) {
            vfill(x0 + mx - o, y0 + my + o, ch - my - o, fg);
            vfill(x0 + mx + o, y0 + my - o, ch - my + o, fg);
          }
        } else if (rnd) {
          /* Rounded corners: draw arcs */
          int r = (ch > 16) ? 3 : 2;
          if (flags & 1)
            hfill(x0, y0 + my, mx - r + 1, fg);
          if (flags & 2)
            hfill(x0 + mx + r, y0 + my, pw - mx - r, fg);
          if (flags & 4)
            vfill(x0 + mx, y0, my - r + 1, fg);
          if (flags & 8)
            vfill(x0 + mx, y0 + my + r, ch - my - r, fg);
          /* Bend pixels */
          if (flags == 10) {
            *fb_px(x0 + mx + 1, y0 + my + 1) = fg;
            *fb_px(x0 + mx + 2, y0 + my + 2) = fg;
          } /* UL */
          if (flags == 9) {
            *fb_px(x0 + mx - 1, y0 + my + 1) = fg;
            *fb_px(x0 + mx - 2, y0 + my + 2) = fg;
          } /* UR */
          if (flags == 6) {
            *fb_px(x0 + mx + 1, y0 + my - 1) = fg;
            *fb_px(x0 + mx + 2, y0 + my - 2) = fg;
          } /* LL */
          if (flags == 5) {
            *fb_px(x0 + mx - 1, y0 + my - 1) = fg;
            *fb_px(x0 + mx - 2, y0 + my - 2) = fg;
          } /* LR */
        } else {
          /* Single lines */
          if (flags & 1)
            hfill(x0, y0 + my, mx + 1, fg);
          if (flags & 2)
            hfill(x0 + mx, y0 + my, pw - mx, fg);
          if (flags & 4)
            vfill(x0 + mx, y0, my + 1, fg);
          if (flags & 8)
            vfill(x0 + mx, y0 + my, ch - my, fg);
        }
        goto cell_done;
      }

      /*  Block elements U+2580-U+2588  */
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

      /*  FreeType glyph  */
      {
        const Glyph *g = font_glyph(code);
        if (__builtin_expect(g->px != NULL, 1)) {
          const int gx = x0 + g->bx;
          const int gy = y0 + bl - g->by;
          int y_start = (gy < y0) ? y0 - gy : 0;
          int y_end = (gy + g->bh > y0 + ch) ? y0 + ch - gy : g->bh;
          int x_start = (gx < x0) ? x0 - gx : 0;
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

  /*  Cursor: pinned to original logical row even when scrolling  */
  if (t->cursor_visible) {
    int screen_cy = (t->total_rows - t->rows + t->cy) - t->view_row;
    if (screen_cy >= 0 && screen_cy < t->rows && t->cx >= 0 &&
        t->cx < t->cols) {
      int x0 = t->cx * cw + MARGIN_LEFT;
      int y0 = screen_cy * ch + MARGIN_TOP;
      uint32_t clr = palette[CURSOR_COLOR];
      for (int y = 0; y < ch; y++)
        hfill(x0, y0 + y, cw, clr);
    }
  }

  if (any)
    display_kick(d);
}

void display_blank(DisplayDev *d, bool blank) {
  if (d->is_drm) {
#if USE_CRTC_BLANK
    drm_blank_crtc(d, blank);
#endif
    if (blank) {
      if (g_fb && d->buf.size > 0)
        memset(g_fb, 0, d->buf.size);
      display_kick(d);
    }
  } else {
    fbdev_blank(d, blank);
  }
}

/*  display_kick  */
void display_kick(DisplayDev *d) {
  if (d->is_drm)
    drm_kick(d);
  else
    fbdev_kick(d);
}

/*  display_free  *
 *  Correct teardown order:
 *    1. vt_restore  – back to KD_TEXT / VT_AUTO before touching hardware.
 *    2. drm_free_dev / fbdev munmap+close – release hardware resources.
 *    3. Null out cached globals.
 *    4. font_free.
 */
void display_free(DisplayDev *d) {
  vt_restore();

  if (d->is_drm) {
    drm_free_dev(d); /* frees shadow inside drm_buf_free */
  } else {
    if (d->buf.map && d->buf.map != MAP_FAILED) {
      munmap(d->buf.map, d->buf.size);
      d->buf.map = NULL;
    }
    if (d->fd >= 0) {
      close(d->fd);
      d->fd = -1;
    }
  }

  g_fb = NULL;
  g_fb_w = g_fb_h = 0;
  font_free();
}

/*  ensure_node  */
static void ensure_node(const char *path, int maj, int min) {
  struct stat st;
  if (stat(path, &st) == 0 && S_ISCHR(st.st_mode) &&
      major(st.st_rdev) == (unsigned)maj && minor(st.st_rdev) == (unsigned)min)
    return;
  (void)mknod(path, S_IFCHR | 0666, makedev((unsigned)maj, (unsigned)min));
}

bool display_init(DisplayDev *d) {
  if (!font_init(&d->cell_w, &d->cell_h)) {
    LOG("font_init failed");
    return false;
  }

  /* Try DRM first. */
  ensure_node(DRM_DEVICE, DRM_MAJOR, DRM_MINOR);
  if (drm_init_dev(d)) {
    d->is_drm = true;
    drm_kickstart(d);
  } else {
    /* Fall back to fbdev. */
    ensure_node(FB_DEVICE, FB_MAJOR, FB_MINOR);
    if (!fbdev_init(d)) {
      font_free();
      return false;
    }
    d->is_drm = false;
    backlight_wake();
  }

  /* For DRM with shadow: render to shadow buffer (tear-free).
   * For DRM without shadow or fbdev: render directly to scanout. */
  g_stride = d->buf.pitch / sizeof(uint32_t);
#if USE_SHADOW_BUFFER
  g_fb = d->is_drm ? d->shadow : d->buf.map;
#else
  g_fb = d->buf.map;
#endif
  g_fb_w = d->width;
  g_fb_h = d->height;

  /*  Build 256-colour xterm palette  */
  for (int i = 0; i < 256; i++) {
    uint8_t r = 0, g2 = 0, b = 0;
    if (i < 16) {
      /* Standard 16 colours: low 8 are dark, high 8 are bright. */
      r = (i & 1) ? ((i > 8) ? 255 : 170) : 0;
      g2 = (i & 2) ? ((i > 8) ? 255 : 170) : 0;
      b = (i & 4) ? ((i > 8) ? 255 : 170) : 0;
      if (i == 0) {
        r = g2 = b = 0;
      }
      if (i == 7) {
        r = g2 = b = 192;
      }
      if (i == 8) {
        r = g2 = b = 85;
      }
    } else if (i < 232) {
      /* 6x6x6 colour cube (16-231). */
      int idx = i - 16;
      int ri = idx / 36, gi = (idx / 6) % 6, bi = idx % 6;
      r = ri ? (uint8_t)(ri * 40 + 55) : 0;
      g2 = gi ? (uint8_t)(gi * 40 + 55) : 0;
      b = bi ? (uint8_t)(bi * 40 + 55) : 0;
    } else {
      /* Greyscale ramp (232-255). */
      r = g2 = b = (uint8_t)((i - 232) * 10 + 8);
    }
#if COLOR_BGR
    palette[i] =
        0xFF000000u | (uint32_t)r | ((uint32_t)g2 << 8) | ((uint32_t)b << 16);
#else
    palette[i] =
        0xFF000000u | (uint32_t)b | ((uint32_t)g2 << 8) | ((uint32_t)r << 16);
#endif
  }

  return true;
}
