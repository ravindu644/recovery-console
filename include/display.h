#ifndef DISPLAY_H
#define DISPLAY_H

#include "term.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  int fd;
  int width, height;
  int cell_w, cell_h; /* derived from font metrics */
  bool is_drm;

  struct {
    uint32_t *map;
    uint32_t size;
    uint32_t pitch;
    uint32_t fb_id;
    uint32_t handle;
  } buf;
} DisplayDev;

bool display_init(DisplayDev *d);
void display_render(DisplayDev *d, Term *t);
void display_blank(DisplayDev *d, bool blank);
void display_kick(DisplayDev *d);
void display_free(DisplayDev *d);

/* Backend-specific (internal) */
bool drm_init_dev(DisplayDev *d);
void drm_free_dev(DisplayDev *d);
bool fbdev_init(DisplayDev *d);

#endif
