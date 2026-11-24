#ifndef LAUNCH_WEB_H
#define LAUNCH_WEB_H

#include "launch.h"

typedef struct {
    char *url, *label, *detail, *profile, *error;
} WebTarget;

bool web_target(GHashTable **bangs, const Settings *, const char *query, WebTarget *);
void web_target_clear(WebTarget *);
char **web_command(const Settings *, Display *, const char *profile, const char *url,
                   GSocket **connection, char **error);
bool web_send(GSocket *, char **argv, GError **);
void web_focus(const Settings *, const char *profile);

#endif
