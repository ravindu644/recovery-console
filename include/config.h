#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Cell height in pixels; width is derived from font metrics */
#define FONT_SIZE 16

#define MARGIN_TOP 0
#define MARGIN_BOTTOM 0
#define MARGIN_LEFT 60
#define MARGIN_RIGHT 0
#define ROTATION 1 /* 0=0deg, 1=90deg, 2=180deg, 3=270deg CW */

/* VGA palette defaults (indices into 256-color palette) */
#define DEFAULT_FG 7
#define DEFAULT_BG 0
#define CURSOR_COLOR 15

/* Display format: 0=RGB, 1=BGR */
#define COLOR_BGR 1

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
  "/sys/devices/platform/soc/1a00000.qcom,mdss_mdp/"                           \
  "1a00000.qcom,mdss_mdp:qcom,mdss_fb_primary/leds/lcd-backlight/brightness"
#define BACKLIGHT_VAL 150

#define LOG(fmt, ...) fprintf(stderr, "[rc] " fmt "\n", ##__VA_ARGS__)

#endif
