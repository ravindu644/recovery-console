#define _GNU_SOURCE
#include "font.h"
#include "config.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <string.h>

/* Open-addressing hash table for glyph cache */
#define CACHE_BITS 13
#define CACHE_SZ (1 << CACHE_BITS)
#define CACHE_MASK (CACHE_SZ - 1)

typedef struct {
  uint32_t cp;
  uint32_t gen; /* generation counter for eviction ordering */
  bool used;
  Glyph g;
} Slot;

static FT_Library g_ft;
static FT_Face g_face;
static int g_cell_w, g_cell_h, g_baseline;
static Slot g_cache[CACHE_SZ];
static unsigned g_used;
static uint32_t g_gen;
static Glyph g_fallback = {.adv = 0, .px = NULL};

/* Multiplicative Fibonacci hash */
static uint32_t cp_hash(uint32_t cp) {
  return (cp * 2654435761u) >> (32 - CACHE_BITS);
}

int font_wcwidth(uint32_t cp) {
  if (cp < 0x300)
    return 1;
  /* Wide Unicode ranges: CJK, Hangul, fullwidth, emoji blocks */
  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
      (cp >= 0x3040 && cp <= 0x33FF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
      (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F004 && cp <= 0x1F9FF) ||
      (cp >= 0x20000 && cp <= 0x2FFFD) || (cp >= 0x30000 && cp <= 0x3FFFD))
    return 2;
  return 1;
}

int font_baseline(void) { return g_baseline; }

static void build_tofu(void);

#ifdef HAVE_EMBEDDED_FONT
#include "font_data.h"
#endif

bool font_init(int *cw, int *ch) {
  if (FT_Init_FreeType(&g_ft))
    return false;

#ifdef HAVE_EMBEDDED_FONT
  if (FT_New_Memory_Face(g_ft, font_ttf, (FT_Long)font_ttf_len, 0, &g_face))
    return false;
#else
  return false; /* No font provided at compile time */
#endif

  if (FT_Set_Pixel_Sizes(g_face, 0, FONT_SIZE)) {
    FT_Done_Face(g_face);
    FT_Done_FreeType(g_ft);
    return false;
  }

  FT_Size_Metrics *m = &g_face->size->metrics;
  g_cell_h = (int)((m->ascender - m->descender) >> 6);
  g_cell_w = (int)(m->max_advance >> 6);
  g_baseline = (int)(m->ascender >> 6);
  if (g_cell_w < 1)
    g_cell_w = g_cell_h / 2;

  *cw = g_cell_w;
  *ch = g_cell_h;
  build_tofu();
  return true;
}

void font_free(void) {
  for (int i = 0; i < CACHE_SZ; i++) {
    if (g_cache[i].used) {
      free(g_cache[i].g.px);
      g_cache[i].used = false;
    }
  }
  free(g_fallback.px);
  g_fallback.px = NULL;
  if (g_face) {
    FT_Done_Face(g_face);
    g_face = NULL;
  }
  if (g_ft) {
    FT_Done_FreeType(g_ft);
    g_ft = NULL;
  }
  g_used = 0;
  g_gen = 0;
}

static Glyph *cache_get(uint32_t cp) {
  uint32_t h = cp_hash(cp);
  for (uint32_t i = 0; i < CACHE_SZ; i++) {
    uint32_t idx = (h + i) & CACHE_MASK;
    if (!g_cache[idx].used)
      return NULL;
    if (g_cache[idx].cp == cp) {
      g_cache[idx].gen = ++g_gen;
      return &g_cache[idx].g;
    }
  }
  return NULL;
}

static Glyph *cache_alloc(uint32_t cp) {
  /* Evict oldest quarter on high load */
  if (g_used >= (CACHE_SZ * 3 / 4)) {
    /* Find generation threshold: evict slots older than median gen */
    uint32_t oldest = g_gen;
    for (int i = 0; i < CACHE_SZ; i++)
      if (g_cache[i].used && g_cache[i].gen < oldest)
        oldest = g_cache[i].gen;
    uint32_t threshold = oldest + (g_gen - oldest) / 2;
    int evicted = 0;
    for (int i = 0; i < CACHE_SZ && evicted < (int)g_used / 4; i++) {
      if (g_cache[i].used && g_cache[i].gen <= threshold) {
        free(g_cache[i].g.px);
        g_cache[i].used = false;
        evicted++;
      }
    }
    g_used -= (unsigned)evicted;
  }
  uint32_t h = cp_hash(cp);
  for (uint32_t i = 0; i < CACHE_SZ; i++) {
    uint32_t idx = (h + i) & CACHE_MASK;
    if (!g_cache[idx].used) {
      g_cache[idx].cp = cp;
      g_cache[idx].gen = ++g_gen;
      g_cache[idx].used = true;
      g_used++;
      return &g_cache[idx].g;
    }
  }
  return NULL;
}

/* Build a visible "tofu" replacement glyph (a small box outline) */

static void build_tofu(void) {
  int w = g_cell_w > 2 ? g_cell_w - 2 : 1;
  int h = g_cell_h > 4 ? g_cell_h - 4 : 1;
  uint8_t *px = calloc((size_t)(w * h), 1);
  if (!px)
    return;
  /* Draw outline rectangle */
  for (int x = 0; x < w; x++) {
    px[x] = 200;
    px[(h - 1) * w + x] = 200;
  }
  for (int y = 0; y < h; y++) {
    px[y * w] = 200;
    px[y * w + w - 1] = 200;
  }
  g_fallback.bw = w;
  g_fallback.bh = h;
  g_fallback.bx = 1;
  g_fallback.by = g_baseline - 2;
  g_fallback.adv = g_cell_w;
  g_fallback.px = px;
}

const Glyph *font_glyph(uint32_t cp) {
  Glyph *g = cache_get(cp);
  if (g)
    return g;

  FT_UInt idx = FT_Get_Char_Index(g_face, cp);
  if (!idx) {
    /* Try the replacement character, then fall back to blank */
    if (cp != 0xFFFD)
      return font_glyph(0xFFFD);
    return &g_fallback;
  }

  if (FT_Load_Glyph(g_face, idx, FT_LOAD_DEFAULT) ||
      FT_Render_Glyph(g_face->glyph, FT_RENDER_MODE_NORMAL))
    return &g_fallback;

  FT_GlyphSlot sl = g_face->glyph;
  FT_Bitmap *bm = &sl->bitmap;

  g = cache_alloc(cp);
  if (!g)
    return &g_fallback;

  g->cp = cp;
  g->bx = sl->bitmap_left;
  g->by = sl->bitmap_top;
  g->adv = (int)(sl->advance.x >> 6);
  g->bw = (int)bm->width;
  g->bh = (int)bm->rows;
  g->px = NULL;

  if (g->bw > 0 && g->bh > 0) {
    g->px = malloc((size_t)(g->bw * g->bh));
    if (!g->px)
      return &g_fallback;
    for (int row = 0; row < g->bh; row++)
      memcpy(g->px + row * g->bw, bm->buffer + row * bm->pitch, (size_t)g->bw);
  }
  return g;
}
