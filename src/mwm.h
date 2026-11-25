#ifndef LAUNCH_MWM_H
#define LAUNCH_MWM_H

#include "launch.h"

bool mwm_running(Display *);
bool mwm_load(GPtrArray *, const char *client, char **error);
void mwm_add_command(GPtrArray *, const char *client, const char *label,
                     const char *kind, const char *const *args);
bool mwm_run(char **argv, char **output, char **error);

#endif
