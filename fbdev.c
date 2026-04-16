#define _GNU_SOURCE
#include "config.h"
#include "display.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void sysfs_unblank(const char *path) {
  int fd = open(path, O_WRONLY);
  if (fd < 0)
    return;
  (void)write(fd, "0\n", 2);
  close(fd);
}

bool fbdev_init(DisplayDev *d) {
  if ((d->fd = open(FB_DEVICE, O_RDWR | O_CLOEXEC)) < 0)
    if ((d->fd = open(FB_DEVICE_ALT, O_RDWR | O_CLOEXEC)) < 0)
      return false;

  sysfs_unblank("/sys/class/graphics/fb0/blank");
  sysfs_unblank("/sys/class/graphics/fb1/blank");

  struct fb_var_screeninfo vi;
  struct fb_fix_screeninfo fi;
  if (ioctl(d->fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
      ioctl(d->fd, FBIOGET_FSCREENINFO, &fi) < 0) {
    close(d->fd);
    return false;
  }

  display_blank(d, false);

  d->width = (int)vi.xres;
  d->height = (int)vi.yres;
  d->buf.pitch = fi.line_length;
  d->buf.size = fi.smem_len;
  d->buf.map =
      mmap(0, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
  if (d->buf.map == MAP_FAILED) {
    close(d->fd);
    return false;
  }

  d->is_drm = false;
  memset(d->buf.map, 0, d->buf.size);
  return true;
}

void fbdev_kick(DisplayDev *d) {
  struct fb_var_screeninfo vi;
  if (ioctl(d->fd, FBIOGET_VSCREENINFO, &vi) == 0) {
    vi.xoffset = vi.yoffset = 0;
    ioctl(d->fd, FBIOPAN_DISPLAY, &vi);
  }
}
