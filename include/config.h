#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Cell height in pixels; width is derived from font metrics */
#define FONT_SIZE 22

#define MARGIN_TOP 95
#define MARGIN_BOTTOM 45
#define MARGIN_LEFT 10
#define MARGIN_RIGHT 10
#define ROTATION 0         /* 0=0deg, 1=90deg, 2=180deg, 3=270deg CW */
#define DISPLAY_TIMEOUT 60 /* seconds of inactivity before sleep */

/* VGA palette defaults (indices into 256-color palette) */
#define DEFAULT_FG 7
#define DEFAULT_BG 0
#define CURSOR_COLOR 15

/* Display format: 0=RGB, 1=BGR */
#define COLOR_BGR 0

/* Shadow buffer: render to malloc'd buffer, memcpy to scanout on kick.
 * Eliminates tearing on video-mode LCD panels. Costs ~10MB extra RAM.
 * Disable (0) for command-mode AMOLED or memory-constrained devices. */
#define USE_SHADOW_BUFFER 1

/* CRTC blank: fully disable CRTC on power-button blank.
 * Required for LCD panels where backlight stays on with memset-only blank.
 * Do NOT enable for AMOLED - CRTC disable/re-enable breaks MTK panels.
 * Default: 0 (memset-to-black, works everywhere). */
#define USE_CRTC_BLANK 0

/* Hardware */
#define DRM_DEVICE "/dev/dri/card0"
#define DRM_MAJOR 226
#define DRM_MINOR 0
#define DRM_CONN_ID 0
#define DRM_CRTC_ID 0

#define FB_DEVICE "/dev/graphics/fb0"
#define FB_DEVICE_ALT "/dev/fb0"
#define FB_MAJOR 29
#define FB_MINOR 0

#define DEFAULT_SHELL "/bin/sh"
#define TERM_ENV "xterm-256color"

#define IO_BUFSZ 32768
#define SELECT_US 10000
#define ESC_BUF_MAX 256
#define CSI_PARAMS_MAX 16
#define SOCKET_PATH "/tmp/rc.sock"

#define BACKLIGHT_PATH                                                         \
  "/sys/devices/platform/soc/soc:mtk_leds/leds/lcd-backlight/brightness"
#define BACKLIGHT_VAL 200

#define LOG(fmt, ...)                                                          \
  do {                                                                         \
    fprintf(stderr, "[rc] " fmt "\n", ##__VA_ARGS__);                          \
    fflush(stderr);                                                            \
  } while (0)

#endif
