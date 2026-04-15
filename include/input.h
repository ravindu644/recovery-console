#ifndef INPUT_H
#define INPUT_H

#include <linux/input.h>
#include <stdbool.h>

#define MAX_INPUTS 8

typedef struct {
  int fds[MAX_INPUTS];
  int count;
} InputDev;

bool input_init(InputDev *in);
void input_free(InputDev *in);
// Returns key code if event occurred, otherwise 0
int input_read(InputDev *in, struct input_event *ev);

#endif
