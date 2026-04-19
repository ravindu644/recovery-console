#ifndef DISPLAY_H
#define DISPLAY_H

#include "term.h"
#include <drm/drm_mode.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  int fd;
  int width, height;
  int cell_w, cell_h; /* derived from font metrics */
  bool is_drm;

  uint32_t *shadow;      /* CPU render target (malloc'd, never scanned out) */
  uint32_t mode_blob_id; /* MODE_ID blob for atomic modeset */
  struct drm_mode_modeinfo saved_mode; /* for CRTC re-program on unblank */

  struct {
    uint32_t *map; /* scanout buffer (DRM mmap or fbdev mmap) */
    uint32_t size;
    uint32_t pitch;
    uint32_t fb_id;
    uint32_t handle;
    uint32_t conn_id;
    uint32_t crtc_id;
    uint32_t primary_plane_id;

    /* Property IDs for Atomic KMS */
    struct {
      uint32_t crtc_active;
      uint32_t crtc_mode_id;
      uint32_t plane_fb_id;
      uint32_t plane_crtc_id;
      uint32_t plane_src_x, plane_src_y, plane_src_w, plane_src_h;
      uint32_t plane_crtc_x, plane_crtc_y, plane_crtc_w, plane_crtc_h;
    } props;
  } buf;
} DisplayDev;

/*  Public display API  */
bool display_init(DisplayDev *d);
void display_render(DisplayDev *d, Term *t);
void display_blank(DisplayDev *d, bool blank);
void display_kick(DisplayDev *d);
void display_free(DisplayDev *d);
void backlight_set(int val);

/*  VT switching support
 *
 *  Lifecycle:
 *    vt_init()          – open active VT, set KD_GRAPHICS, register
 *                         VT_PROCESS. Works for BOTH DRM and fbdev.
 *    vt_release()       – SIGUSR1 deferred: drop DRM master (DRM) or
 *                         just stop rendering (fbdev), then ack kernel.
 *    vt_acquire()       – SIGUSR2 deferred: re-grab master, full-clear
 *                         framebuffer (kills margin ghosts), active=1.
 *    vt_restore()       – restore KD_TEXT + VT_AUTO on exit.
 *    vt_get_fd()        – VT fd for select() exceptfds (hard detach).
 */
int vt_init(DisplayDev *d);
void vt_restore(void);
void vt_release(DisplayDev *d);
void vt_acquire(DisplayDev *d);
int vt_get_fd(void);

extern volatile sig_atomic_t g_vt_active; /* 1 = we own the display */

/*  Backend-specific (internal)  */
bool drm_init_dev(DisplayDev *d);
void drm_free_dev(DisplayDev *d);
void drm_drop_master(DisplayDev *d);
void drm_set_master(DisplayDev *d);
void drm_kick(DisplayDev *d);
void drm_reprogram_crtc(DisplayDev *d);
void drm_blank_crtc(DisplayDev *d, bool blank);
void drm_set_power(DisplayDev *d, bool on);
void drm_kickstart(DisplayDev *d);

bool fbdev_init(DisplayDev *d);
void fbdev_blank(DisplayDev *d, bool blank);
void fbdev_kick(DisplayDev *d);

#endif /* DISPLAY_H */
