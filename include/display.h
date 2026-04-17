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

/* VT switching support */
int vt_init(DisplayDev *d); /* open active VT fd, register VT_PROCESS mode */
void vt_restore(void);      /* restore VT_AUTO mode on exit */
void vt_release(DisplayDev *d); /* SIGUSR1: drop DRM master, ack VT release */
void vt_acquire(DisplayDev *d); /* SIGUSR2: re-acquire DRM master, re-render */

/* Backend-specific (internal) */
bool drm_init_dev(DisplayDev *d);
void drm_free_dev(DisplayDev *d);
void drm_drop_master(DisplayDev *d);
void drm_set_master(DisplayDev *d);
bool fbdev_init(DisplayDev *d);

#endif
