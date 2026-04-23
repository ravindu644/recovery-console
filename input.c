#define _GNU_SOURCE
#include "input.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define INPUT_DEBUG 0

static const char key_normal[KEY_MAX] = {
    [KEY_GRAVE] = '`',       [KEY_1] = '1',          [KEY_2] = '2',
    [KEY_3] = '3',           [KEY_4] = '4',          [KEY_5] = '5',
    [KEY_6] = '6',           [KEY_7] = '7',          [KEY_8] = '8',
    [KEY_9] = '9',           [KEY_0] = '0',          [KEY_MINUS] = '-',
    [KEY_EQUAL] = '=',       [KEY_Q] = 'q',          [KEY_W] = 'w',
    [KEY_E] = 'e',           [KEY_R] = 'r',          [KEY_T] = 't',
    [KEY_Y] = 'y',           [KEY_U] = 'u',          [KEY_I] = 'i',
    [KEY_O] = 'o',           [KEY_P] = 'p',          [KEY_LEFTBRACE] = '[',
    [KEY_RIGHTBRACE] = ']',  [KEY_BACKSLASH] = '\\', [KEY_A] = 'a',
    [KEY_S] = 's',           [KEY_D] = 'd',          [KEY_F] = 'f',
    [KEY_G] = 'g',           [KEY_H] = 'h',          [KEY_J] = 'j',
    [KEY_K] = 'k',           [KEY_L] = 'l',          [KEY_SEMICOLON] = ';',
    [KEY_APOSTROPHE] = '\'', [KEY_Z] = 'z',          [KEY_X] = 'x',
    [KEY_C] = 'c',           [KEY_V] = 'v',          [KEY_B] = 'b',
    [KEY_N] = 'n',           [KEY_M] = 'm',          [KEY_COMMA] = ',',
    [KEY_DOT] = '.',         [KEY_SLASH] = '/',      [KEY_SPACE] = ' ',
    [KEY_KP0] = '0',         [KEY_KP1] = '1',        [KEY_KP2] = '2',
    [KEY_KP3] = '3',         [KEY_KP4] = '4',        [KEY_KP5] = '5',
    [KEY_KP6] = '6',         [KEY_KP7] = '7',        [KEY_KP8] = '8',
    [KEY_KP9] = '9',         [KEY_KPDOT] = '.',      [KEY_KPPLUS] = '+',
    [KEY_KPMINUS] = '-',     [KEY_KPASTERISK] = '*', [KEY_KPSLASH] = '/',
    [KEY_KPENTER] = '\r',
};

static const char key_shifted[KEY_MAX] = {
    [KEY_GRAVE] = '~',      [KEY_1] = '!',         [KEY_2] = '@',
    [KEY_3] = '#',          [KEY_4] = '$',         [KEY_5] = '%',
    [KEY_6] = '^',          [KEY_7] = '&',         [KEY_8] = '*',
    [KEY_9] = '(',          [KEY_0] = ')',         [KEY_MINUS] = '_',
    [KEY_EQUAL] = '+',      [KEY_Q] = 'Q',         [KEY_W] = 'W',
    [KEY_E] = 'E',          [KEY_R] = 'R',         [KEY_T] = 'T',
    [KEY_Y] = 'Y',          [KEY_U] = 'U',         [KEY_I] = 'I',
    [KEY_O] = 'O',          [KEY_P] = 'P',         [KEY_LEFTBRACE] = '{',
    [KEY_RIGHTBRACE] = '}', [KEY_BACKSLASH] = '|', [KEY_A] = 'A',
    [KEY_S] = 'S',          [KEY_D] = 'D',         [KEY_F] = 'F',
    [KEY_G] = 'G',          [KEY_H] = 'H',         [KEY_J] = 'J',
    [KEY_K] = 'K',          [KEY_L] = 'L',         [KEY_SEMICOLON] = ':',
    [KEY_APOSTROPHE] = '"', [KEY_Z] = 'Z',         [KEY_X] = 'X',
    [KEY_C] = 'C',          [KEY_V] = 'V',         [KEY_B] = 'B',
    [KEY_N] = 'N',          [KEY_M] = 'M',         [KEY_COMMA] = '<',
    [KEY_DOT] = '>',        [KEY_SLASH] = '?',     [KEY_SPACE] = ' ',
};

#include <sys/inotify.h>
#include <time.h>

static uint64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/*
 * Try to open and register one evdev node.
 *
 * - Dedup by nodename: silently skip if already tracked.
 * - Retry open up to MAX_OPEN_TRIES times with OPEN_RETRY_MS delay to
 *   handle the kernel/udev race where IN_CREATE fires before the node
 *   is ready to open (common on USB attach).
 * - Accept any device with EV_KEY capability (keyboards, vol keys,
 *   power key) — same policy as before, just with dedup + retry.
 */
#define MAX_OPEN_TRIES 5
#define OPEN_RETRY_MS  50

static void input_add_device(InputDev *in, const char *path) {
  if (in->count >= MAX_INPUTS)
    return;

  const char *slash = strrchr(path, '/');
  const char *nodename = slash ? slash + 1 : path;

  /* Dedup: skip if already tracking this node */
  for (int i = 0; i < in->count; i++) {
    if (strcmp(in->nodenames[i], nodename) == 0)
      return;
  }

  /* Retry open to handle udev/kernel node-creation race */
  int fd = -1;
  for (int t = 0; t < MAX_OPEN_TRIES && fd < 0; t++) {
    if (t > 0)
      usleep(OPEN_RETRY_MS * 1000);
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  }
  if (fd < 0)
    return;

  /* Filter: only devices that can emit key events */
  unsigned long evbits[(EV_MAX / (sizeof(unsigned long) * 8)) + 1] = {0};
  if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 ||
      !(evbits[0] & (1UL << EV_KEY))) {
    close(fd);
    return;
  }

  char devname[64] = "<unknown>";
  ioctl(fd, EVIOCGNAME(sizeof(devname)), devname);

  in->fds[in->count] = fd;
  snprintf(in->devnames[in->count], sizeof(in->devnames[0]), "%s", devname);
  snprintf(in->nodenames[in->count], sizeof(in->nodenames[0]), "%s", nodename);
  in->count++;
}

void input_remove_device(InputDev *in, int index) {
  if (!in || index < 0 || index >= in->count)
    return;
  close(in->fds[index]);
  if (index < in->count - 1) {
    int n = in->count - index - 1;
    memmove(&in->fds[index], &in->fds[index + 1], (size_t)n * sizeof(int));
    memmove(&in->devnames[index], &in->devnames[index + 1], (size_t)n * 64);
    memmove(&in->nodenames[index], &in->nodenames[index + 1], (size_t)n * 32);
  }
  in->count--;
}

bool input_init(InputDev *in) {
  if (!in)
    return false;
  in->count = 0;
  in->shift = in->ctrl = in->alt = in->capslock = false;
  in->rescan_at_ms = 0;

  /* Set up inotify BEFORE readdir so no IN_CREATE events are missed */
  in->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (in->inotify_fd >= 0) {
    in->watch_fd =
        inotify_add_watch(in->inotify_fd, "/dev/input", IN_CREATE | IN_DELETE);
  }

  DIR *dir = opendir("/dev/input");
  if (!dir) {
    if (in->inotify_fd >= 0)
      close(in->inotify_fd);
    return false;
  }

  struct dirent *de;
  while ((de = readdir(dir))) {
    if (strncmp(de->d_name, "event", 5) == 0) {
      char path[PATH_MAX];
      snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
      input_add_device(in, path);
    }
  }
  closedir(dir);

  /*
   * Schedule a deferred re-scan 2 seconds after init.
   *
   * USB keyboards attached at boot may not have their evdev nodes in
   * /dev/input until after the kernel finishes USB enumeration, which
   * often races with the readdir above.  inotify IN_CREATE only fires
   * for nodes created after the watch is registered, so nodes that appear
   * in this window are caught automatically.  The re-scan is a safety net
   * for nodes that appear in the window between process start and inotify
   * watch registration, or on platforms where uevents are delayed.
   *
   * input_rescan() uses dedup in input_add_device so double-registration
   * is impossible.
   */
  in->rescan_at_ms = mono_ms() + 2000;

  return true;
}

/*
 * Full re-scan of /dev/input.  Safe to call at any time; dedup in
 * input_add_device prevents double-registration.  Called from the main
 * loop when rescan_at_ms is reached, and can also be triggered manually.
 */
void input_rescan(InputDev *in) {
  if (!in)
    return;
  in->rescan_at_ms = 0;

  DIR *dir = opendir("/dev/input");
  if (!dir)
    return;

  struct dirent *de;
  while ((de = readdir(dir))) {
    if (strncmp(de->d_name, "event", 5) == 0) {
      char path[PATH_MAX];
      snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
      input_add_device(in, path);
    }
  }
  closedir(dir);
}


void input_handle_hotplug(InputDev *in) {
  uint8_t buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  ssize_t len;

  while ((len = read(in->inotify_fd, buf, sizeof(buf))) > 0) {
    for (uint8_t *ptr = buf; ptr < buf + len;) {
      const struct inotify_event *event = (const struct inotify_event *)ptr;
      if (event->len && (strncmp(event->name, "event", 5) == 0)) {
        if (event->mask & IN_CREATE) {
          char path[PATH_MAX];
          snprintf(path, sizeof(path), "/dev/input/%s", event->name);
          input_add_device(in, path);
        } else if (event->mask & IN_DELETE) {
          /* Proactive removal */
          for (int i = 0; i < in->count; i++) {
            if (strcmp(in->nodenames[i], event->name) == 0) {
              input_remove_device(in, i);
              break;
            }
          }
        }
      }
      ptr += sizeof(struct inotify_event) + event->len;
    }
  }
}

void input_free(InputDev *in) {
  if (!in)
    return;
  while (in->count > 0)
    input_remove_device(in, 0);
  if (in->inotify_fd >= 0)
    close(in->inotify_fd);
  in->inotify_fd = -1;
}

/* Drain events accumulated in the kernel ring while VT was inactive */
void input_flush(InputDev *in) {
  struct input_event ev;
  for (int i = 0; i < in->count; i++)
    while (read(in->fds[i], &ev, sizeof(ev)) > 0);
}

int input_read(InputDev *in, struct input_event *ev, int *out_idx) {
  for (int i = 0; i < in->count; i++) {
    ssize_t n = read(in->fds[i], ev, sizeof(*ev));
    if (n == sizeof(*ev)) {
      if (out_idx)
        *out_idx = i;
      return 1;
    } else if (n <= 0 && errno != EAGAIN) {
      input_remove_device(in, i);
      i--;
    }
  }
  return 0;
}

int input_ev_to_pty(InputDev *in, const struct input_event *ev, int pty_fd) {
  if (ev->type != EV_KEY)
    return 0;

  int code = ev->code;
  int value = ev->value;

#if INPUT_DEBUG
  char dbg[64];
  int dlen =
      snprintf(dbg, sizeof(dbg), "\r\n[ev:%d v:%d s:%d c:%d a:%d caps:%d]\r\n",
               code, value, in->shift, in->ctrl, in->alt, in->capslock);
  (void)write(pty_fd, dbg, (size_t)dlen);
#endif

  switch (code) {
  case KEY_LEFTSHIFT:
  case KEY_RIGHTSHIFT:
    in->shift = (value != 0);
    return 0;
  case KEY_LEFTCTRL:
  case KEY_RIGHTCTRL:
    in->ctrl = (value != 0);
    return 0;
  case KEY_LEFTALT:
  case KEY_RIGHTALT:
    in->alt = (value != 0);
    return 0;
  case KEY_CAPSLOCK:
    if (value == 1)
      in->capslock = !in->capslock;
    return 0;
  }

  if (value == 0)
    return 0;

  const char *seq = NULL;
  switch (code) {
  case KEY_UP:
    seq = in->alt ? "\033\033[A" : "\033[A";
    break;
  case KEY_DOWN:
    seq = in->alt ? "\033\033[B" : "\033[B";
    break;
  case KEY_RIGHT:
    seq = in->alt ? "\033\033[C" : "\033[C";
    break;
  case KEY_LEFT:
    seq = in->alt ? "\033\033[D" : "\033[D";
    break;
  case KEY_HOME:
    seq = "\033[H";
    break;
  case KEY_END:
    seq = "\033[F";
    break;
  case KEY_INSERT:
    seq = "\033[2~";
    break;
  case KEY_DELETE:
    seq = "\033[3~";
    break;
  case KEY_PAGEUP:
    seq = "\033[5~";
    break;
  case KEY_PAGEDOWN:
    seq = "\033[6~";
    break;
  case KEY_F1:
    seq = "\033OP";
    break;
  case KEY_F2:
    seq = "\033OQ";
    break;
  case KEY_F3:
    seq = "\033OR";
    break;
  case KEY_F4:
    seq = "\033OS";
    break;
  case KEY_F5:
    seq = "\033[15~";
    break;
  case KEY_F6:
    seq = "\033[17~";
    break;
  case KEY_F7:
    seq = "\033[18~";
    break;
  case KEY_F8:
    seq = "\033[19~";
    break;
  case KEY_F9:
    seq = "\033[20~";
    break;
  case KEY_F10:
    seq = "\033[21~";
    break;
  case KEY_F11:
    seq = "\033[23~";
    break;
  case KEY_F12:
    seq = "\033[24~";
    break;
  case KEY_ESC:
    seq = "\033";
    break;
  case KEY_TAB:
    seq = in->shift ? "\033[Z" : "\t";
    break;
  case KEY_ENTER:
  case KEY_KPENTER:
    seq = "\r";
    break;
  case KEY_BACKSPACE:
    seq = "\x7f";
    break;
  }
  if (seq)
    return (int)write(pty_fd, seq, strlen(seq));

  if (code >= KEY_MAX)
    return 0;
  char base = key_normal[code];
  if (!base)
    return 0;

  bool effective_shift = in->shift;
  if (in->capslock && base >= 'a' && base <= 'z')
    effective_shift = !effective_shift;

  char ch = effective_shift ? key_shifted[code] : base;
  if (!ch)
    return 0;

  if (in->ctrl) {
    char upper = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    if ((upper >= '@' && upper <= '_') || upper == '?') {
      char ctrl_ch = (upper == '?') ? '\x7f' : (char)(upper & 0x1f);
      if (in->alt) {
        char buf[2] = {'\033', ctrl_ch};
        return (int)write(pty_fd, buf, 2);
      }
      return (int)write(pty_fd, &ctrl_ch, 1);
    }
  }

  if (in->alt) {
    char buf[2] = {'\033', ch};
    return (int)write(pty_fd, buf, 2);
  }

  return (int)write(pty_fd, &ch, 1);
}
