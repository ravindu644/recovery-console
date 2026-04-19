#include "term.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HIST_MAX 512

/* VT100 alternate character set: maps 0x60-0x7E to Unicode box/misc */
static const uint32_t vt100_acs[31] = {
    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1,
    0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA,
    0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C,
    0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

static inline Cell *cell_at(Term *t, int r, int c) {
  return &t->cells[r * t->cols + c];
}

static void row_clear(Term *t, int r, int c0, int c1) {
  if (r < 0 || r >= HIST_MAX)
    return;
  int end = c1 < t->cols - 1 ? c1 : t->cols - 1;
  for (int c = c0; c <= end; c++) {
    Cell *cl = cell_at(t, r, c);
    cl->code = ' ';
    cl->fg = t->fg;
    cl->bg = t->bg;
    cl->attr = 0;
    cl->width = 1;
  }
  if (r >= t->view_row && r < t->view_row + t->rows)
    t->dirty[r - t->view_row] = true;
}

static void scroll_up(Term *t, int top, int bot, int n) {
  if (n <= 0)
    return;
  if (top == 0 && bot == t->rows - 1 && !t->use_alt_screen) {
    for (int i = 0; i < n; i++) {
      if (t->view_row + t->rows < HIST_MAX) {
        t->view_row++;
        if (t->view_row + t->rows > t->total_rows)
          t->total_rows = t->view_row + t->rows;
        row_clear(t, t->view_row + t->rows - 1, 0, t->cols - 1);
      } else {
        memmove(t->cells, t->cells + t->cols,
                sizeof(Cell) * (size_t)(t->cols * (HIST_MAX - 1)));
        row_clear(t, HIST_MAX - 1, 0, t->cols - 1);
      }
    }
    t->screen_dirty = true;
    return;
  }
  for (int r = top; r <= bot - n; r++) {
    memcpy(cell_at(t, t->view_row + r, 0), cell_at(t, t->view_row + r + n, 0),
           sizeof(Cell) * (size_t)t->cols);
    t->dirty[r] = true;
  }
  for (int r = bot - n + 1; r <= bot; r++)
    row_clear(t, t->view_row + r, 0, t->cols - 1);
}

static void scroll_down(Term *t, int top, int bot, int n) {
  if (n <= 0)
    return;
  for (int r = bot; r >= top + n; r--) {
    memcpy(cell_at(t, t->view_row + r, 0), cell_at(t, t->view_row + r - n, 0),
           sizeof(Cell) * (size_t)t->cols);
    t->dirty[r] = true;
  }
  for (int r = top; r < top + n; r++)
    row_clear(t, t->view_row + r, 0, t->cols - 1);
}

/* -------------------------------------------------------------------------
 * Unified cursor engine -- ported from st (suckless terminal).
 *
 * tmoveto(t, x, y)
 *   The ONE function all cursor positioning goes through.
 *   - Clamps x to [0, cols-1], y to [0, rows-1]  (absolute screen coords,
 *     not relative to scroll region -- matching st / xterm default mode)
 *   - Clears CURSOR_WRAPNEXT so position always reflects the real cell
 *
 * CURSOR_WRAPNEXT (deferred-wrap / pending-wrap flag)
 *   Set when a printable char lands on the very last column.
 *   On the NEXT printable char we do the newline FIRST, then write.
 *   This is how every real terminal (xterm, st, foot, kitty) works.
 *   Without it cx appears to be cols (off-screen), breaking all absolute
 *   positioning math (APT progress bar, vim status line, etc.)
 *
 * tnewline(t, first_col)
 *   Handles LF / IND / autowrap.  Scrolls iff cy == scroll_bot.
 * ------------------------------------------------------------------------- */

/* Move cursor to (x, y) in absolute screen coords.
 * Clears the pending-wrap flag.  This is the only way cx/cy change. */
static void tmoveto(Term *t, int x, int y) {
  if (x < 0)
    x = 0;
  if (x >= t->cols)
    x = t->cols - 1;
  if (y < 0)
    y = 0;
  if (y >= t->rows)
    y = t->rows - 1;
  t->cursor_wrapnext = false;
  t->cx = x;
  t->cy = y;
}

/* New-line: advance cy one row, scrolling if at scroll_bot.
 * first_col: if true, also move cx to 0 (like NEL/CR+LF). */
static void tnewline(Term *t, bool first_col) {
  int y = t->cy;
  if (y == t->scroll_bot) {
    scroll_up(t, t->scroll_top, t->scroll_bot, 1);
  } else {
    y++;
  }
  tmoveto(t, first_col ? 0 : t->cx, y);
}

void term_init(Term *t, int px_w, int px_h, int cell_w, int cell_h) {
  int w = px_w, h = px_h;
  if (ROTATION % 2 == 1) {
    w = px_h;
    h = px_w;
  }
  w -= (MARGIN_LEFT + MARGIN_RIGHT);
  h -= (MARGIN_TOP + MARGIN_BOTTOM);
  t->cell_w = cell_w;
  t->cell_h = cell_h;
  t->cols = w / cell_w;
  t->rows = h / cell_h;
  if (t->cols < 1)
    t->cols = 1;
  if (t->rows < 1)
    t->rows = 1;
  t->total_rows = t->rows;
  t->view_row = 0;
  t->scroll_top = 0;
  t->scroll_bot = t->rows - 1;
  t->saved_scroll_top = 0;
  t->saved_scroll_bot = t->rows - 1;
  t->saved_charset_gfx = false;
  t->fg = DEFAULT_FG;
  t->bg = DEFAULT_BG;
  t->cursor_visible = true;
  t->mode_autowrap = true; /* Standard VT100 behavior */
  t->cells = calloc((size_t)(t->cols * HIST_MAX), sizeof(Cell));
  t->cells_alt = calloc((size_t)(t->cols * t->rows), sizeof(Cell));
  t->dirty = calloc((size_t)t->rows, sizeof(bool));
  t->utf8_code = 0;
  t->utf8_expect = 0;
  if (!t->cells || !t->cells_alt || !t->dirty) {
    free(t->cells);
    free(t->cells_alt);
    free(t->dirty);
    t->cells = t->cells_alt = NULL;
    t->dirty = NULL;
    LOG("term_init: out of memory");
    return;
  }
  for (int r = 0; r < t->total_rows; r++)
    row_clear(t, r, 0, t->cols - 1);
}

void term_free(Term *t) {
  free(t->cells);
  free(t->cells_alt);
  free(t->dirty);
}

/* Write one Unicode codepoint at cursor, advance with deferred-wrap.
 * st model:
 *   1. If CURSOR_WRAPNEXT is pending, do the newline NOW then write.
 *   2. Write the glyph at (cx, cy).
 *   3. If cx+w < cols: advance cx normally via tmoveto.
 *      Else: set CURSOR_WRAPNEXT (do NOT advance past last column).
 */
static void putchar_at(Term *t, uint32_t code) {
  int w = font_wcwidth(code);
  if (w < 1)
    w = 1;

  /* Pending wrap: materialise the newline before writing this char */
  if (t->cursor_wrapnext) {
    /* autowrap: go to col 0 of next row */
    tnewline(t, true);
  }

  /* Clamp write position (safety -- tmoveto / tnewline should keep us sane) */
  if (t->cx + w > t->cols)
    tmoveto(t, t->cols - w, t->cy);

  Cell *c = cell_at(t, t->view_row + t->cy, t->cx);
  c->code = code;
  c->fg = t->fg;
  c->bg = t->bg;
  c->attr = t->attr;
  c->width = (uint8_t)w;
  t->dirty[t->cy] = true;

  /* Continuation cell for double-wide glyphs */
  if (w == 2 && t->cx + 1 < t->cols) {
    Cell *c2 = cell_at(t, t->view_row + t->cy, t->cx + 1);
    c2->code = 0;
    c2->fg = t->fg;
    c2->bg = t->bg;
    c2->attr = t->attr;
    c2->width = 0;
  }

  /* Advance cursor or arm deferred-wrap */
  if (t->cx + w < t->cols) {
    tmoveto(t, t->cx + w, t->cy); /* normal advance -- clears WRAPNEXT */
  } else {
    /* At last column: do NOT move cx past the edge.
     * Set WRAPNEXT so the NEXT printable char triggers the newline. */
    t->cx = t->cols - w; /* keep cx at last valid cell */
    t->cursor_wrapnext = t->mode_autowrap;
  }
}

static int param(Term *t, int idx, int def) {
  int v = (idx < t->nparams) ? t->params[idx] : 0;
  return v ? v : def;
}

static void csi_dispatch(Term *t, char fin) {
  int old_cy = t->cy;
  int p0 = param(t, 0, 1);
  int p1 = param(t, 1, 1);
  int rp0 = t->nparams > 0 ? t->params[0] : 0;

  switch (fin) {
  case 'A': /* CUU -- cursor up */
    tmoveto(t, t->cx, t->cy - p0);
    break;
  case 'B': /* CUD -- cursor down */
    tmoveto(t, t->cx, t->cy + p0);
    break;
  case 'C': /* CUF -- cursor forward */
    tmoveto(t, t->cx + p0, t->cy);
    break;
  case 'D': /* CUB -- cursor backward */
    tmoveto(t, t->cx - p0, t->cy);
    break;
  case 'E': /* CNL -- cursor next line */
    tmoveto(t, 0, t->cy + p0);
    break;
  case 'F': /* CPL -- cursor prev line */
    tmoveto(t, 0, t->cy - p0);
    break;
  case 'G': /* CHA -- cursor horizontal absolute */
    tmoveto(t, p0 - 1, t->cy);
    break;
  case 'H': /* CUP -- cursor position */
  case 'f': /* HVP -- horizontal and vertical position */
    tmoveto(t, p1 - 1, p0 - 1);
    break;
  case 'd': /* VPA -- vertical line position absolute */
    tmoveto(t, t->cx, p0 - 1);
    break;
  case 'J':
    if (rp0 == 1)
      for (int r = 0; r <= t->cy && r < t->rows; r++)
        row_clear(t, t->view_row + r, 0, r == t->cy ? t->cx : t->cols - 1);
    else if (rp0 >= 2)
      for (int r = 0; r < t->rows; r++)
        row_clear(t, t->view_row + r, 0, t->cols - 1);
    else {
      row_clear(t, t->view_row + t->cy, t->cx, t->cols - 1);
      for (int r = t->cy + 1; r < t->rows; r++)
        row_clear(t, t->view_row + r, 0, t->cols - 1);
    }
    break;
  case 'K':
    if (rp0 == 1)
      row_clear(t, t->view_row + t->cy, 0, t->cx);
    else if (rp0 == 2)
      row_clear(t, t->view_row + t->cy, 0, t->cols - 1);
    else
      row_clear(t, t->view_row + t->cy, t->cx, t->cols - 1);
    break;
  case 'L':
    scroll_down(t, t->cy, t->scroll_bot, p0);
    break;
  case 'M':
    scroll_up(t, t->cy, t->scroll_bot, p0);
    break;
  case 'S':
    scroll_up(t, t->scroll_top, t->scroll_bot, p0);
    break;
  case 'T':
    scroll_down(t, t->scroll_top, t->scroll_bot, p0);
    break;
  case 'P': {
    int n = p0 > t->cols - t->cx ? t->cols - t->cx : p0;
    memmove(cell_at(t, t->view_row + t->cy, t->cx),
            cell_at(t, t->view_row + t->cy, t->cx + n),
            sizeof(Cell) * (size_t)(t->cols - t->cx - n));
    row_clear(t, t->view_row + t->cy, t->cols - n, t->cols - 1);
    break;
  }
  case 'X':
    row_clear(t, t->view_row + t->cy, t->cx, t->cx + p0 - 1);
    break;
  case 'r': /* DECSTBM -- set top and bottom margins */
    t->scroll_top = p0 - 1;
    t->scroll_bot = p1 - 1 < t->rows ? p1 - 1 : t->rows - 1;
    if (t->scroll_top < 0)
      t->scroll_top = 0;
    if (t->scroll_bot <= t->scroll_top)
      t->scroll_bot = t->rows - 1;
    tmoveto(t, 0, 0); /* spec: cursor homes after DECSTBM */
    break;
  case 's': /* save cursor (ANSI) */
    t->saved_cx = t->cx;
    t->saved_cy = t->cy;
    t->saved_fg = t->fg;
    t->saved_bg = t->bg;
    t->saved_attr = t->attr;
    t->saved_scroll_top = t->scroll_top;
    t->saved_scroll_bot = t->scroll_bot;
    t->saved_wrapnext = t->cursor_wrapnext;
    t->saved_charset_gfx = t->charset_gfx;
    break;
  case 'u': /* restore cursor (ANSI) */
    t->fg = t->saved_fg;
    t->bg = t->saved_bg;
    t->attr = t->saved_attr;
    t->scroll_top = t->saved_scroll_top;
    t->scroll_bot = t->saved_scroll_bot;
    t->cursor_wrapnext = t->saved_wrapnext;
    t->charset_gfx = t->saved_charset_gfx;
    tmoveto(t, t->saved_cx, t->saved_cy);
    break;
  case 'm':
    /* SGR: colors and attributes */
    for (int i = 0; i < (t->nparams ? t->nparams : 1); i++) {
      int v = t->nparams ? t->params[i] : 0;

      if (v == 0) {
        t->fg = DEFAULT_FG;
        t->bg = DEFAULT_BG;
        t->attr = 0;
      } else if (v == 1) {
        t->attr |= ATTR_BOLD;
      } else if (v == 2) {
        t->attr |= ATTR_DIM;
      } else if (v == 3) {
        t->attr |= ATTR_ITALIC;
      } else if (v == 4) {
        t->attr |= ATTR_UNDERLINE;
      } else if (v == 5) {
        t->attr |= ATTR_BLINK;
      } else if (v == 7) {
        t->attr |= ATTR_REVERSE;
      } else if (v == 22) {
        t->attr &= ~(ATTR_BOLD | ATTR_DIM);
      } else if (v == 23) {
        t->attr &= ~ATTR_ITALIC;
      } else if (v == 24) {
        t->attr &= ~ATTR_UNDERLINE;
      } else if (v == 25) {
        t->attr &= ~ATTR_BLINK;
      } else if (v == 27) {
        t->attr &= ~ATTR_REVERSE;
      } else if (v >= 30 && v <= 37) {
        t->fg = (uint32_t)(v - 30);
      } else if (v == 39) {
        t->fg = DEFAULT_FG;
      } else if (v >= 40 && v <= 47) {
        t->bg = (uint32_t)(v - 40);
      } else if (v == 49) {
        t->bg = DEFAULT_BG;
      } else if (v >= 90 && v <= 97) {
        t->fg = (uint32_t)(v - 90 + 8);
      } else if (v >= 100 && v <= 107) {
        t->bg = (uint32_t)(v - 100 + 8);
      }
      /* 256-color */
      else if ((v == 38 || v == 48) && i + 2 < t->nparams &&
               t->params[i + 1] == 5) {
        uint32_t idx = (uint32_t)(t->params[i + 2] & 0xFF);
        if (v == 38)
          t->fg = idx;
        else
          t->bg = idx;
        i += 2;
      }
      /* True color (24-bit) */
      else if ((v == 38 || v == 48) && i + 4 < t->nparams &&
               t->params[i + 1] == 2) {
        uint32_t rgb =
            MKRGB(t->params[i + 2], t->params[i + 3], t->params[i + 4]);
        if (v == 38)
          t->fg = rgb;
        else
          t->bg = rgb;
        i += 4;
      }
    }
    break;
  case 'h':
    if (t->priv && rp0 == 25)
      t->cursor_visible = true;
    if (t->priv && rp0 == 7)
      t->mode_autowrap = true;
    if (t->priv && rp0 == 1049 && !t->use_alt_screen) {
      Cell *tmp = t->cells;
      t->cells = t->cells_alt;
      t->cells_alt = tmp;
      t->use_alt_screen = true;
      t->saved_cx = t->cx;
      t->saved_cy = t->cy;
      for (int r = 0; r < t->rows; r++)
        row_clear(t, r, 0, t->cols - 1);
    }
    break;
  case 'l':
    if (t->priv && rp0 == 25) {
      t->cursor_visible = false;
      t->dirty[t->cy] = true;
    }
    if (t->priv && rp0 == 7)
      t->mode_autowrap = false;
    if (t->priv && rp0 == 1049 && t->use_alt_screen) {
      Cell *tmp = t->cells;
      t->cells = t->cells_alt;
      t->cells_alt = tmp;
      t->use_alt_screen = false;
      t->cx = t->saved_cx;
      t->cy = t->saved_cy;
      t->scroll_top = 0;
      t->scroll_bot = t->rows - 1;
      for (int r = 0; r < t->rows; r++)
        t->dirty[r] = true;
    }
    break;
  case 'n': { /* DSR - device status report */
    char resp[32];
    int len = 0;
    if (rp0 == 5) { /* device OK */
      len = snprintf(resp, sizeof(resp), "\033[0n");
    } else if (rp0 == 6) { /* CPR - cursor position report */
      len = snprintf(resp, sizeof(resp), "\033[%d;%dR", t->cy + 1, t->cx + 1);
    }
    if (len > 0 && t->pty_fd >= 0)
      (void)write(t->pty_fd, resp, (size_t)len);
    break;
  }
  case '@': { /* ICH - insert characters */
    int n = p0 > t->cols - t->cx ? t->cols - t->cx : p0;
    memmove(cell_at(t, t->view_row + t->cy, t->cx + n),
            cell_at(t, t->view_row + t->cy, t->cx),
            sizeof(Cell) * (size_t)(t->cols - t->cx - n));
    row_clear(t, t->view_row + t->cy, t->cx, t->cx + n - 1);
    break;
  }
  }

  t->dirty[old_cy] = true;
  t->dirty[t->cy] = true;
}

void term_write(Term *t, const uint8_t *buf, int n) {
  for (int i = 0; i < n; i++) {
    uint8_t b = buf[i];

    /* UTF-8 decode */
    if (t->utf8_expect > 0 && (b & 0xC0) == 0x80) {
      t->utf8_code = (t->utf8_code << 6) | (b & 0x3F);
      if (--t->utf8_expect > 0)
        continue;
    } else if (b >= 0xC0) {
      if (b < 0xE0) {
        t->utf8_code = b & 0x1F;
        t->utf8_expect = 1;
      } else if (b < 0xF0) {
        t->utf8_code = b & 0x0F;
        t->utf8_expect = 2;
      } else {
        t->utf8_code = b & 0x07;
        t->utf8_expect = 3;
      }
      continue;
    } else {
      t->utf8_code = b;
      t->utf8_expect = 0;
    }

    uint32_t ch = t->utf8_code;

    switch (t->state) {
    case ST_GROUND:
      if (ch == 0x1b) {
        t->state = ST_ESC;
        t->esclen = 0;
      } else if (ch == '\r') {
        /* CR: move to column 0, keep row */
        tmoveto(t, 0, t->cy);
      } else if (ch == '\n' || ch == 0x0b || ch == 0x0c) {
        /* LF / VT / FF: st calls tnewline(0) -- advance row, keep col */
        t->dirty[t->cy] = true;
        tnewline(t, false);
      } else if (ch == '\b') {
        /* BS: move left one col */
        tmoveto(t, t->cx - 1, t->cy);
      } else if (ch == '\t') {
        /* HT: next tab stop */
        t->dirty[t->cy] = true;
        int nx = (t->cx + 8) & ~7;
        if (nx >= t->cols)
          nx = t->cols - 1;
        tmoveto(t, nx, t->cy);
      } else if (ch == 0x07 || ch == 0x0e || ch == 0x0f) {
        /* BEL, SO, SI - ignore */
      } else if (ch >= 0x20) {
        uint32_t code = ch;
        /* VT100 ACS: map 0x60-0x7E to real Unicode box chars */
        if (t->charset_gfx && ch >= 0x60 && ch <= 0x7E)
          code = vt100_acs[ch - 0x60];
        putchar_at(t, code);
      }
      break;

    case ST_ESC:
      if (ch == '[') {
        t->state = ST_CSI;
        t->esclen = 0;
        t->nparams = 0;
        t->priv = false;
      } else if (ch == '(') {
        t->state = ST_ESC_INTERMEDIATE; /* next byte selects charset */
      } else if (ch == ')') {
        t->state = ST_ESC_INTERMEDIATE; /* G1 charset, skip */
      } else if (ch == ']') {
        t->state = ST_OSC;
      } else if (ch == 'M') {
        /* RI -- Reverse Index: scroll down if at scroll_top, else move up */
        t->dirty[t->cy] = true;
        if (t->cy == t->scroll_top)
          scroll_down(t, t->scroll_top, t->scroll_bot, 1);
        else
          tmoveto(t, t->cx, t->cy - 1);
        t->dirty[t->cy] = true;
        t->state = ST_GROUND;
      } else if (ch == '7') {
        /* DECSC save cursor */
        t->saved_cx = t->cx;
        t->saved_cy = t->cy;
        t->saved_fg = t->fg;
        t->saved_bg = t->bg;
        t->saved_attr = t->attr;
        t->saved_scroll_top = t->scroll_top;
        t->saved_scroll_bot = t->scroll_bot;
        t->saved_wrapnext = t->cursor_wrapnext;
        t->saved_charset_gfx = t->charset_gfx;
        t->state = ST_GROUND;
      } else if (ch == '8') {
        /* DECRC restore cursor */
        t->fg = t->saved_fg;
        t->bg = t->saved_bg;
        t->attr = t->saved_attr;
        t->scroll_top = t->saved_scroll_top;
        t->scroll_bot = t->saved_scroll_bot;
        t->cursor_wrapnext = t->saved_wrapnext;
        t->charset_gfx = t->saved_charset_gfx;
        tmoveto(t, t->saved_cx, t->saved_cy);
        t->state = ST_GROUND;
      } else if (ch == 'c') {
        /* RIS - full reset */
        t->fg = DEFAULT_FG;
        t->bg = DEFAULT_BG;
        t->attr = 0;
        t->scroll_top = 0;
        t->scroll_bot = t->rows - 1;
        t->charset_gfx = false;
        t->cursor_wrapnext = false;
        tmoveto(t, 0, 0);
        for (int r = 0; r < t->rows; r++)
          row_clear(t, t->view_row + r, 0, t->cols - 1);
        t->state = ST_GROUND;
      } else {
        t->state = ST_GROUND;
      }
      break;

    case ST_ESC_INTERMEDIATE:
      /* Charset designation: '0'=graphics, 'B'=ASCII, others ignore */
      t->charset_gfx = (ch == '0');
      t->state = ST_GROUND;
      break;

    case ST_CSI:
      if (ch == '?') {
        t->priv = true;
      } else if (ch >= 0x40 && ch <= 0x7E) {
        /* Parse accumulated params */
        t->nparams = 0;
        int cur = 0;
        bool got = false;
        for (int j = 0; j < t->esclen; j++) {
          char ec = t->escbuf[j];
          if (ec >= '0' && ec <= '9') {
            cur = cur * 10 + (ec - '0');
            got = true;
          } else if (ec == ';') {
            if (t->nparams < CSI_PARAMS_MAX)
              t->params[t->nparams++] = cur;
            cur = 0;
            got = false;
          }
        }
        if (got && t->nparams < CSI_PARAMS_MAX)
          t->params[t->nparams++] = cur;
        csi_dispatch(t, (char)ch);
        t->state = ST_GROUND;
      } else if (t->esclen < ESC_BUF_MAX - 1) {
        t->escbuf[t->esclen++] = (char)ch;
      }
      break;

    case ST_OSC:
      /* Consume until ST (ESC \) or BEL */
      if (ch == 0x07 ||
          (ch == 0x1b && t->esclen > 0 && t->escbuf[t->esclen - 1] == '\\'))
        t->state = ST_GROUND;
      else if (ch == 0x1b) {
        /* Could be start of ESC \ string terminator */
        if (t->esclen < ESC_BUF_MAX - 1)
          t->escbuf[t->esclen++] = (char)ch;
      }
      break;
    }
  }
}

void term_scroll(Term *t, int delta) {
  if (t->use_alt_screen)
    return;
  int old = t->view_row;
  t->view_row += delta;
  if (t->view_row < 0)
    t->view_row = 0;
  if (t->view_row > t->total_rows - t->rows)
    t->view_row = t->total_rows - t->rows;
  if (t->view_row != old)
    t->screen_dirty = true;
}

void term_snap_to_bottom(Term *t) {
  if (t->use_alt_screen)
    return;
  int target = t->total_rows - t->rows;
  if (target < 0)
    target = 0;
  if (t->view_row != target) {
    t->view_row = target;
    t->screen_dirty = true;
  }
}
