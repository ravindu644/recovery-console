#define _GNU_SOURCE
#include "config.h"
#include "display.h"
#include <dirent.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

bool drm_init_dev(DisplayDev *d) {
  d->fd = -1;
  if ((d->fd = open(DRM_DEVICE, O_RDWR | O_CLOEXEC)) < 0)
    return false;

  struct drm_set_client_cap cp = {.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES,
                                  .value = 1};
  ioctl(d->fd, DRM_IOCTL_SET_CLIENT_CAP, &cp);

  struct drm_mode_card_res res = {0};
  if (ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
    goto fail_fd;

  uint32_t *crtcs = calloc(res.count_crtcs, sizeof(uint32_t));
  uint32_t *conns = calloc(res.count_connectors, sizeof(uint32_t));
  uint32_t *encs = calloc(res.count_encoders, sizeof(uint32_t));
  if (!crtcs || !conns || !encs) {
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }
  res.crtc_id_ptr = (uintptr_t)crtcs;
  res.connector_id_ptr = (uintptr_t)conns;
  res.encoder_id_ptr = (uintptr_t)encs;
  if (ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }

  /* Pick best connected connector (prefer DSI/eDP for mobile) */
  uint32_t best_c = DRM_CONN_ID, used_crtc = DRM_CRTC_ID;
  if (!best_c) {
    int best_s = -1;
    for (uint32_t i = 0; i < res.count_connectors; i++) {
      struct drm_mode_get_connector gc = {.connector_id = conns[i]};
      if (ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0 ||
          gc.connection != 1)
        continue;
      int s = (gc.connector_type == DRM_MODE_CONNECTOR_DSI)   ? 4
              : (gc.connector_type == DRM_MODE_CONNECTOR_eDP) ? 3
                                                              : 1;
      if (s > best_s) {
        best_s = s;
        best_c = conns[i];
      }
    }
  }
  if (!best_c) {
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }

  d->is_drm = true;
  struct drm_mode_get_connector con = {.connector_id = best_c};
  ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &con);
  uint32_t sv_enc = con.encoder_id;

  if (con.count_modes == 0) {
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }
  struct drm_mode_modeinfo *ms = calloc(con.count_modes, sizeof(*ms));
  if (!ms) {
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }
  con.modes_ptr = (uintptr_t)ms;
  con.count_props = con.count_encoders = 0;
  if (ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &con) < 0) {
    free(ms);
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }

  int midx = 0;
  for (uint32_t i = 0; i < con.count_modes; i++)
    if (ms[i].type & DRM_MODE_TYPE_PREFERRED) {
      midx = (int)i;
      break;
    }

  d->width = ms[midx].hdisplay;
  d->height = ms[midx].vdisplay;

  if (!used_crtc) {
    struct drm_mode_get_encoder ge = {.encoder_id = sv_enc};
    if (sv_enc && ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &ge) == 0)
      used_crtc = ge.crtc_id;
    if (!used_crtc && res.count_crtcs > 0)
      used_crtc = crtcs[0];
  }

  struct drm_mode_create_dumb cr = {
      .width = (uint32_t)d->width, .height = (uint32_t)d->height, .bpp = 32};
  if (ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr) < 0) {
    free(ms);
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }
  d->buf.handle = cr.handle;
  d->buf.pitch = cr.pitch;
  d->buf.size = cr.size;

  struct drm_mode_fb_cmd fb = {.width = (uint32_t)d->width,
                               .height = (uint32_t)d->height,
                               .pitch = d->buf.pitch,
                               .bpp = 32,
                               .depth = 24,
                               .handle = d->buf.handle};
  if (ioctl(d->fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
    free(ms);
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }
  d->buf.fb_id = fb.fb_id;

  struct drm_mode_map_dumb mq = {.handle = d->buf.handle};
  if (ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &mq) < 0) {
    free(ms);
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }
  d->buf.map = mmap(NULL, d->buf.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    d->fd, (off_t)mq.offset);
  if (d->buf.map == MAP_FAILED) {
    d->buf.map = NULL;
    free(ms);
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }
  memset(d->buf.map, 0, d->buf.size);

  /* Acquire master only long enough to program the CRTC, then drop it so
   * startx / another DRM client can take over on a VT switch. */
  ioctl(d->fd, DRM_IOCTL_SET_MASTER, 0);
  struct drm_mode_crtc cc = {.crtc_id = used_crtc,
                             .fb_id = d->buf.fb_id,
                             .set_connectors_ptr = (uintptr_t)&best_c,
                             .count_connectors = 1,
                             .mode_valid = 1,
                             .mode = ms[midx]};
  if (ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &cc) < 0) {
    munmap(d->buf.map, d->buf.size);
    d->buf.map = NULL;
    free(ms);
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }

  /* Drop master immediately; vt_acquire() will re-grab when we are active. */
  ioctl(d->fd, DRM_IOCTL_DROP_MASTER, 0);

  free(ms);
  free(crtcs);
  free(conns);
  free(encs);
  return true;

fail_fd:
  close(d->fd);
  d->fd = -1;
  return false;
}

void drm_free_dev(DisplayDev *d) {
  if (d->buf.map && d->buf.map != MAP_FAILED)
    munmap(d->buf.map, d->buf.size);
  if (d->fd >= 0)
    close(d->fd);
}

void drm_kick(DisplayDev *d) {
  struct drm_mode_fb_dirty_cmd dy = {.fb_id = d->buf.fb_id};
  ioctl(d->fd, 0xC01064B1 /* DRM_IOCTL_MODE_DIRTYFB */, &dy);
}

void drm_drop_master(DisplayDev *d) {
  if (d->fd >= 0)
    ioctl(d->fd, DRM_IOCTL_DROP_MASTER, 0);
}

void drm_set_master(DisplayDev *d) {
  if (d->fd >= 0)
    ioctl(d->fd, DRM_IOCTL_SET_MASTER, 0);
}
