#define _GNU_SOURCE
#include "input.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

bool input_init(InputDev *in) {
  in->count = 0;
  for (int i = 0; i < 32 && in->count < MAX_INPUTS; i++) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
      continue;

    unsigned long evbits[(EV_MAX / (sizeof(unsigned long) * 8)) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
      close(fd);
      continue;
    }

    if (!(evbits[0] & (1UL << EV_KEY))) {
      close(fd);
      continue;
    }

    in->fds[in->count++] = fd;
  }
  return in->count > 0;
}

void input_free(InputDev *in) {
  for (int i = 0; i < in->count; i++)
    close(in->fds[i]);
  in->count = 0;
}

int input_read(InputDev *in, struct input_event *ev) {
  for (int i = 0; i < in->count; i++) {
    if (read(in->fds[i], ev, sizeof(*ev)) == sizeof(*ev))
      return 1;
  }
  return 0;
}
