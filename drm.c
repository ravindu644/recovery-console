#define _GNU_SOURCE
#include "config.h"
#include "display.h"
#include <dirent.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef DRM_IOCTL_MODE_DIRTYFB
#define DRM_IOCTL_MODE_DIRTYFB DRM_IOWR(0xB1, struct drm_mode_fb_dirty_cmd)
#endif

#ifndef DRM_IOCTL_MODE_DESTROY_DUMB
struct drm_mode_destroy_dumb {
  __u32 handle;
};
#define DRM_IOCTL_MODE_DESTROY_DUMB DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)
#endif

/* Fourcc for the scanout framebuffer, controlled by COLOR_BGR. */
#if COLOR_BGR
#define FB_FOURCC DRM_FORMAT_XBGR8888
#else
#define FB_FOURCC DRM_FORMAT_XRGB8888
#endif

/* Find a DRM property ID by name on an object. */
static uint32_t get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type,
                            const char *name) {
  struct drm_mode_obj_get_properties prop_res = {.obj_id = obj_id,
                                                 .obj_type = obj_type};
  if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &prop_res) < 0)
    return 0;

  uint32_t *props = calloc(prop_res.count_props, sizeof(uint32_t));
  uint64_t *prop_vals = calloc(prop_res.count_props, sizeof(uint64_t));
  if (!props || !prop_vals) {
    free(props);
    free(prop_vals);
    return 0;
  }
  prop_res.props_ptr = (uintptr_t)props;
  prop_res.prop_values_ptr = (uintptr_t)prop_vals;

  uint32_t ret_id = 0;
  if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &prop_res) == 0) {
    for (uint32_t i = 0; i < prop_res.count_props; i++) {
      struct drm_mode_get_property gp = {.prop_id = props[i]};
      if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) == 0) {
        if (strcmp(gp.name, name) == 0) {
          ret_id = props[i];
          break;
        }
      }
    }
  }
  free(props);
  free(prop_vals);
  return ret_id;
}

/* Write backlight sysfs node. */
void backlight_set(int val) {
  FILE *bf = fopen(BACKLIGHT_PATH, "w");
  if (bf) {
    fprintf(bf, "%d\n", val);
    fclose(bf);
  }
}

/* Release GEM dumb buffer + shadow. */
static void drm_buf_free(DisplayDev *d) {
  if (d->buf.map && d->buf.map != MAP_FAILED) {
    munmap(d->buf.map, d->buf.size);
    d->buf.map = NULL;
  }
#if USE_SHADOW_BUFFER
  free(d->shadow);
  d->shadow = NULL;
#endif
  if (d->fd < 0)
    return;
  if (d->buf.fb_id) {
    ioctl(d->fd, DRM_IOCTL_MODE_RMFB, &d->buf.fb_id);
    d->buf.fb_id = 0;
  }
  if (d->buf.handle) {
    struct drm_mode_destroy_dumb dd = {.handle = d->buf.handle};
    ioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    d->buf.handle = 0;
  }
}

/* ---- Atomic commit helper ----
 * Packs (obj_id, prop_id, value) triples into the flat arrays
 * required by DRM_IOCTL_MODE_ATOMIC. Handles up to 3 objects with
 * up to 12 properties total (plane + crtc + connector). */
#define ATOMIC_MAX_OBJS 3
#define ATOMIC_MAX_PROPS 12

typedef struct {
  uint32_t obj_ids[ATOMIC_MAX_OBJS];
  uint32_t obj_prop_counts[ATOMIC_MAX_OBJS];
  uint32_t prop_ids[ATOMIC_MAX_PROPS];
  uint64_t prop_vals[ATOMIC_MAX_PROPS];
  int n_objs;
  int n_props;
  int cur_obj; /* index into obj_ids for current object being built */
} AtomicReq;

static void atomic_begin(AtomicReq *a) {
  memset(a, 0, sizeof(*a));
  a->cur_obj = -1;
}

static void atomic_obj(AtomicReq *a, uint32_t obj_id) {
  a->cur_obj = a->n_objs++;
  a->obj_ids[a->cur_obj] = obj_id;
  a->obj_prop_counts[a->cur_obj] = 0;
}

static void atomic_prop(AtomicReq *a, uint32_t prop_id, uint64_t val) {
  if (!prop_id || a->cur_obj < 0)
    return;
  a->prop_ids[a->n_props] = prop_id;
  a->prop_vals[a->n_props] = val;
  a->n_props++;
  a->obj_prop_counts[a->cur_obj]++;
}

static int atomic_commit(int fd, AtomicReq *a, uint32_t flags) {
  struct drm_mode_atomic req = {
      .flags = flags,
      .count_objs = (uint32_t)a->n_objs,
      .objs_ptr = (uintptr_t)a->obj_ids,
      .count_props_ptr = (uintptr_t)a->obj_prop_counts,
      .props_ptr = (uintptr_t)a->prop_ids,
      .prop_values_ptr = (uintptr_t)a->prop_vals,
  };
  return ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &req);
}

/* ---- Resolve all plane geometry properties ---- */
static void resolve_plane_props(int fd, DisplayDev *d, uint32_t plane_id) {
  d->buf.props.plane_fb_id =
      get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
  d->buf.props.plane_crtc_id =
      get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
  d->buf.props.plane_src_x =
      get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
  d->buf.props.plane_src_y =
      get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
  d->buf.props.plane_src_w =
      get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
  d->buf.props.plane_src_h =
      get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
  d->buf.props.plane_crtc_x =
      get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
  d->buf.props.plane_crtc_y =
      get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
  d->buf.props.plane_crtc_w =
      get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
  d->buf.props.plane_crtc_h =
      get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
}

/* ---- Atomic full modeset: connector+crtc+plane in one commit ---- */
static int atomic_modeset(DisplayDev *d) {
  if (!d->buf.props.crtc_active || !d->buf.props.crtc_mode_id ||
      !d->buf.primary_plane_id || !d->buf.props.plane_fb_id)
    return -1;

  /* Create mode blob */
  if (d->mode_blob_id) {
    struct drm_mode_destroy_blob db = {.blob_id = d->mode_blob_id};
    ioctl(d->fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &db);
    d->mode_blob_id = 0;
  }
  struct drm_mode_create_blob cb = {.data = (uintptr_t)&d->saved_mode,
                                    .length = sizeof(struct drm_mode_modeinfo)};
  if (ioctl(d->fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &cb) < 0)
    return -1;
  d->mode_blob_id = cb.blob_id;

  AtomicReq a;
  atomic_begin(&a);

  /* Connector object: bind to our CRTC (required for wake-from-sleep) */
  if (d->buf.props.conn_crtc_id) {
    atomic_obj(&a, d->buf.conn_id);
    atomic_prop(&a, d->buf.props.conn_crtc_id, d->buf.crtc_id);
  }

  /* CRTC object */
  atomic_obj(&a, d->buf.crtc_id);
  atomic_prop(&a, d->buf.props.crtc_active, 1);
  atomic_prop(&a, d->buf.props.crtc_mode_id, d->mode_blob_id);

  /* Plane object: attach FB + set source/dest rects */
  atomic_obj(&a, d->buf.primary_plane_id);
  atomic_prop(&a, d->buf.props.plane_fb_id, d->buf.fb_id);
  atomic_prop(&a, d->buf.props.plane_crtc_id, d->buf.crtc_id);
  /* Source rect is in 16.16 fixed point */
  atomic_prop(&a, d->buf.props.plane_src_x, 0);
  atomic_prop(&a, d->buf.props.plane_src_y, 0);
  atomic_prop(&a, d->buf.props.plane_src_w, (uint64_t)d->width << 16);
  atomic_prop(&a, d->buf.props.plane_src_h, (uint64_t)d->height << 16);
  atomic_prop(&a, d->buf.props.plane_crtc_x, 0);
  atomic_prop(&a, d->buf.props.plane_crtc_y, 0);
  atomic_prop(&a, d->buf.props.plane_crtc_w, (uint64_t)d->width);
  atomic_prop(&a, d->buf.props.plane_crtc_h, (uint64_t)d->height);

  return atomic_commit(d->fd, &a, DRM_MODE_ATOMIC_ALLOW_MODESET);
}

/* ---- Atomic page flip: update FB_ID on the plane ---- */
static int atomic_flip(DisplayDev *d) {
  if (!d->buf.primary_plane_id || !d->buf.props.plane_fb_id)
    return -1;

  AtomicReq a;
  atomic_begin(&a);
  atomic_obj(&a, d->buf.primary_plane_id);
  atomic_prop(&a, d->buf.props.plane_fb_id, d->buf.fb_id);

  return atomic_commit(d->fd, &a, 0);
}

/* ---- Add framebuffer: ADDFB2 with fallback to legacy ADDFB ---- */
static int add_fb(int fd, uint32_t w, uint32_t h, uint32_t pitch,
                  uint32_t handle, uint32_t *out_fb_id) {
  /* Try ADDFB2 with explicit fourcc first (what TWRP does) */
  struct drm_mode_fb_cmd2 fb2 = {
      .width = w,
      .height = h,
      .pixel_format = FB_FOURCC,
      .handles = {handle},
      .pitches = {pitch},
      .offsets = {0},
  };
  if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2) == 0) {
    *out_fb_id = fb2.fb_id;
    LOG("FB created via ADDFB2 (fourcc: %c%c%c%c)", FB_FOURCC & 0xFF,
        (FB_FOURCC >> 8) & 0xFF, (FB_FOURCC >> 16) & 0xFF,
        (FB_FOURCC >> 24) & 0xFF);
    return 0;
  }

  /* Fallback: legacy ADDFB (always maps to XRGB8888) */
  struct drm_mode_fb_cmd fb = {
      .width = w,
      .height = h,
      .pitch = pitch,
      .bpp = 32,
      .depth = 24,
      .handle = handle,
  };
  if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) == 0) {
    *out_fb_id = fb.fb_id;
    LOG("FB created via legacy ADDFB (XRGB8888)");
    return 0;
  }

  return -1;
}

bool drm_init_dev(DisplayDev *d) {
  d->fd = -1;

  if ((d->fd = open(DRM_DEVICE, O_RDWR | O_CLOEXEC)) < 0) {
    LOG("failed to open " DRM_DEVICE);
    return false;
  }
  LOG("DRM device opened: " DRM_DEVICE);

  /* Enable universal planes + atomic caps */
  {
    struct drm_set_client_cap cp = {
        .capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1};
    ioctl(d->fd, DRM_IOCTL_SET_CLIENT_CAP, &cp);
  }

  /* Step 1: enumerate resources */
  struct drm_mode_card_res res = {0};
  if (ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
    goto fail_fd;
  if (res.count_connectors == 0 || res.count_crtcs == 0)
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

  /* Step 2: pick best connected connector */
  uint32_t best_conn = DRM_CONN_ID;
  uint32_t used_crtc = DRM_CRTC_ID;
  if (!best_conn) {
    int best_score = -1;
    for (uint32_t i = 0; i < res.count_connectors; i++) {
      struct drm_mode_get_connector gc = {.connector_id = conns[i]};
      if (ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0)
        continue;
      if (gc.connection != 1)
        continue;
      int score = (gc.connector_type == DRM_MODE_CONNECTOR_DSI)    ? 4
                  : (gc.connector_type == DRM_MODE_CONNECTOR_eDP)  ? 3
                  : (gc.connector_type == DRM_MODE_CONNECTOR_LVDS) ? 2
                                                                   : 1;
      if (score > best_score) {
        best_score = score;
        best_conn = conns[i];
      }
    }
  }
  if (!best_conn) {
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }

  /* Step 3: get connector modes + active encoder */
  struct drm_mode_get_connector con = {.connector_id = best_conn};
  if (ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &con) < 0 ||
      con.count_modes == 0) {
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }
  uint32_t saved_enc_id = con.encoder_id;

  struct drm_mode_modeinfo *modes = calloc(con.count_modes, sizeof(*modes));
  if (!modes) {
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }
  con.modes_ptr = (uintptr_t)modes;
  con.count_props = 0;
  con.count_encoders = 0;
  if (ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &con) < 0) {
    free(modes);
    free(crtcs);
    free(conns);
    free(encs);
    goto fail_fd;
  }

  int midx = 0;
  for (uint32_t i = 0; i < con.count_modes; i++) {
    if (modes[i].type & DRM_MODE_TYPE_PREFERRED) {
      midx = (int)i;
      break;
    }
  }
  d->width = (int)modes[midx].hdisplay;
  d->height = (int)modes[midx].vdisplay;
  d->saved_mode = modes[midx];

  /* Step 4: resolve CRTC */
  if (!used_crtc) {
    if (saved_enc_id) {
      struct drm_mode_get_encoder ge = {.encoder_id = saved_enc_id};
      if (ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &ge) == 0 && ge.crtc_id)
        used_crtc = ge.crtc_id;
    }
    if (!used_crtc && saved_enc_id) {
      struct drm_mode_get_encoder ge = {.encoder_id = saved_enc_id};
      ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &ge);
      for (uint32_t i = 0; i < res.count_crtcs; i++) {
        if (ge.possible_crtcs & (1u << i)) {
          used_crtc = crtcs[i];
          break;
        }
      }
    }
    if (!used_crtc && res.count_crtcs > 0)
      used_crtc = crtcs[0];
  }

  free(crtcs);
  free(conns);
  free(encs);

  d->buf.conn_id = best_conn;
  d->buf.crtc_id = used_crtc;

  /* Step 5: allocate dumb buffer */
  struct drm_mode_create_dumb cr = {
      .width = (uint32_t)d->width,
      .height = (uint32_t)d->height,
      .bpp = 32,
  };
  if (ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr) < 0) {
    free(modes);
    goto fail_fd;
  }
  d->buf.handle = cr.handle;
  d->buf.pitch = cr.pitch;
  d->buf.size = (uint32_t)cr.size;

  /* Step 5b: allocate shadow buffer if enabled */
#if USE_SHADOW_BUFFER
  d->shadow = calloc(1, d->buf.size);
  if (!d->shadow) {
    struct drm_mode_destroy_dumb dd = {.handle = d->buf.handle};
    ioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    d->buf.handle = 0;
    free(modes);
    goto fail_fd;
  }
#endif

  /* Step 6: add framebuffer (ADDFB2 with fourcc, fallback legacy) */
  if (add_fb(d->fd, (uint32_t)d->width, (uint32_t)d->height, d->buf.pitch,
             d->buf.handle, &d->buf.fb_id) < 0) {
    struct drm_mode_destroy_dumb dd = {.handle = d->buf.handle};
    ioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    d->buf.handle = 0;
    free(modes);
    goto fail_fd;
  }

  /* Step 7: mmap scanout buffer */
  struct drm_mode_map_dumb mq = {.handle = d->buf.handle};
  if (ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &mq) < 0) {
    drm_buf_free(d);
    free(modes);
    goto fail_fd;
  }
  d->buf.map = mmap(NULL, d->buf.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    d->fd, (off_t)mq.offset);
  if (d->buf.map == MAP_FAILED) {
    d->buf.map = NULL;
    drm_buf_free(d);
    free(modes);
    goto fail_fd;
  }
  memset(d->buf.map, 0, d->buf.size);

  LOG("display: %dx%d (CRTC:%u, Connector:%u)", d->width, d->height,
      d->buf.crtc_id, d->buf.conn_id);

  /* Resolve CRTC atomic properties */
  d->buf.props.crtc_active =
      get_prop_id(d->fd, d->buf.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
  d->buf.props.crtc_mode_id =
      get_prop_id(d->fd, d->buf.crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
  LOG("atomic: CRTC_ACTIVE:%u, CRTC_MODE_ID:%u", d->buf.props.crtc_active,
      d->buf.props.crtc_mode_id);

  /* Resolve connector's CRTC_ID (needed to bind connector in atomic) */
  d->buf.props.conn_crtc_id =
      get_prop_id(d->fd, d->buf.conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");

  /* Find primary plane and resolve all its properties */
  struct drm_mode_get_plane_res pres = {0};
  if (ioctl(d->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) == 0) {
    uint32_t *planes = calloc(pres.count_planes, sizeof(uint32_t));
    pres.plane_id_ptr = (uintptr_t)planes;
    if (ioctl(d->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) == 0) {
      for (uint32_t i = 0; i < pres.count_planes; i++) {
        struct drm_mode_get_plane p = {.plane_id = planes[i]};
        if (ioctl(d->fd, DRM_IOCTL_MODE_GETPLANE, &p) == 0) {
          if (p.crtc_id == d->buf.crtc_id || p.crtc_id == 0) {
            d->buf.primary_plane_id = planes[i];
            resolve_plane_props(d->fd, d, planes[i]);
            LOG("plane: %u (FB_ID:%u, CRTC_ID:%u)", d->buf.primary_plane_id,
                d->buf.props.plane_fb_id, d->buf.props.plane_crtc_id);
            break;
          }
        }
      }
    }
    free(planes);
  }

  /* Step 8: program display - atomic first, legacy fallback */
  ioctl(d->fd, DRM_IOCTL_SET_MASTER, 0);

  if (atomic_modeset(d) == 0) {
    LOG("modeset: atomic commit OK");
  } else {
    /* Legacy SETCRTC fallback */
    struct drm_mode_crtc cc = {
        .crtc_id = used_crtc,
        .fb_id = d->buf.fb_id,
        .set_connectors_ptr = (uintptr_t)&best_conn,
        .count_connectors = 1,
        .mode_valid = 1,
        .mode = modes[midx],
    };
    if (ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &cc) < 0) {
      ioctl(d->fd, DRM_IOCTL_DROP_MASTER, 0);
      drm_buf_free(d);
      free(modes);
      goto fail_fd;
    }
    LOG("modeset: legacy SETCRTC fallback");
  }

  drm_drop_master(d);
  free(modes);
  d->is_drm = true;
  return true;

fail_fd:
  if (d->fd >= 0) {
    close(d->fd);
    d->fd = -1;
  }
  return false;
}

void drm_free_dev(DisplayDev *d) {
  drm_buf_free(d);
  if (d->fd >= 0) {
    close(d->fd);
    d->fd = -1;
  }
}

/* Re-program display after VT acquire or unblank.
 * Tries atomic, falls back to legacy SETCRTC. */
void drm_reprogram_crtc(DisplayDev *d) {
  if (d->fd < 0 || !d->buf.fb_id || !d->buf.crtc_id)
    return;
  drm_set_master(d);

  if (atomic_modeset(d) == 0) {
    drm_drop_master(d);
    return;
  }

  /* Legacy fallback */
  uint32_t conn = d->buf.conn_id;
  struct drm_mode_crtc cc = {
      .crtc_id = d->buf.crtc_id,
      .fb_id = d->buf.fb_id,
      .set_connectors_ptr = (uintptr_t)&conn,
      .count_connectors = 1,
      .mode_valid = 1,
      .mode = d->saved_mode,
  };
  ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &cc);
  drm_drop_master(d);
}

/* Set display power state (on/off). Atomic first, legacy fallback. */
void drm_set_power(DisplayDev *d, bool on) {
  if (d->fd < 0 || !d->is_drm)
    return;

  drm_set_master(d);

  if (d->buf.props.crtc_active) {
    /* Atomic path */
    AtomicReq a;
    atomic_begin(&a);
    atomic_obj(&a, d->buf.crtc_id);
    atomic_prop(&a, d->buf.props.crtc_active, on ? 1 : 0);

    if (on && d->buf.props.crtc_mode_id) {
      if (!d->mode_blob_id) {
        struct drm_mode_create_blob cb = {.data = (uintptr_t)&d->saved_mode,
                                          .length =
                                              sizeof(struct drm_mode_modeinfo)};
        ioctl(d->fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &cb);
        d->mode_blob_id = cb.blob_id;
      }
      atomic_prop(&a, d->buf.props.crtc_mode_id, d->mode_blob_id);
    }

    if (atomic_commit(d->fd, &a, DRM_MODE_ATOMIC_ALLOW_MODESET) == 0)
      goto sync_bl;
  }

  /* Legacy fallback */
  {
    struct drm_mode_crtc cc = {
        .crtc_id = d->buf.crtc_id,
        .fb_id = on ? d->buf.fb_id : 0,
        .set_connectors_ptr = (uintptr_t)&d->buf.conn_id,
        .count_connectors = on ? 1 : 0,
        .mode_valid = on ? 1 : 0,
        .mode = d->saved_mode,
    };
    ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &cc);
  }

sync_bl:
  /* Physical backlight sync */
  backlight_set(on ? BACKLIGHT_VAL : 0);
  drm_drop_master(d);
}

/* TWRP-style power-on kickstart: off -> blank -> unblank -> on. */
void drm_kickstart(DisplayDev *d) {
  backlight_set(BACKLIGHT_VAL); /* 1. probe */
  backlight_set(0);             /* 2. off */
  drm_set_power(d, false);      /* 3. blank */
  drm_set_power(d, true);       /* 4. unblank */
  backlight_set(BACKLIGHT_VAL); /* 5. on */
}

void drm_atomic_set_active(DisplayDev *d, bool active) {
  drm_set_power(d, active);
}

/* Flush shadow -> scanout and tell hardware to update.
 * Tries atomic page flip, falls back to DIRTYFB. */
void drm_kick(DisplayDev *d) {
  if (d->fd < 0 || !d->buf.map)
    return;

  /* Try to grab master; if busy, skip this frame (likely Xorg is active) */
  if (ioctl(d->fd, DRM_IOCTL_SET_MASTER, 0) < 0)
    return;

#if USE_SHADOW_BUFFER
  if (d->shadow)
    memcpy(d->buf.map, d->shadow, d->buf.size);
#endif

  /* Atomic page flip first */
  if (atomic_flip(d) < 0) {
    /* Fallback: DIRTYFB */
    struct drm_mode_fb_dirty_cmd dy = {.fb_id = d->buf.fb_id};
    ioctl(d->fd, DRM_IOCTL_MODE_DIRTYFB, &dy);
  }

  drm_drop_master(d);
}

void drm_blank_crtc(DisplayDev *d, bool blank) { drm_set_power(d, !blank); }

void drm_drop_master(DisplayDev *d) {
  if (d->fd >= 0)
    ioctl(d->fd, DRM_IOCTL_DROP_MASTER, 0);
}

void drm_set_master(DisplayDev *d) {
  if (d->fd >= 0)
    ioctl(d->fd, DRM_IOCTL_SET_MASTER, 0);
}
