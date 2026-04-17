#ifndef INPUT_H
#define INPUT_H

#include <linux/input.h>
#include <stdbool.h>

#define MAX_INPUTS 8

typedef struct {
  int fds[MAX_INPUTS];
  char devnames[MAX_INPUTS][64];
  char nodenames[MAX_INPUTS][32];
  int count;
  int inotify_fd;
  int watch_fd;
  /* modifier state */
  bool shift;
  bool ctrl;
  bool alt;
  bool capslock;
} InputDev;

bool input_init(InputDev *in);
void input_free(InputDev *in);
int input_read(InputDev *in, struct input_event *ev, int *out_idx);
void input_handle_hotplug(InputDev *in);
void input_remove_device(InputDev *in, int index);
int input_ev_to_pty(InputDev *in, const struct input_event *ev, int pty_fd);

#endif
