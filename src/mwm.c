#include <X11/Xatom.h>
#include <string.h>
#include "mwm.h"

bool mwm_running(Display *display)
{
    Atom check = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", True);
    if (check == None)
        return false;

    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;
    Window manager = None;
    if (XGetWindowProperty(display, DefaultRootWindow(display), check, 0, 1, False, XA_WINDOW,
                           &type, &format, &count, &remaining, &data) == Success &&
        type == XA_WINDOW && format == 32 && count == 1 && !remaining)
        manager = *(Window *)data;
    XFree(data);
    if (manager == None)
        return false;

    Atom name = XInternAtom(display, "_NET_WM_NAME", True);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", True);
    if (name == None || utf8 == None)
        return false;
    data = NULL;
    bool running = XGetWindowProperty(display, manager, name, 0, 1, False, utf8,
                                      &type, &format, &count, &remaining, &data) == Success &&
                   type == utf8 && format == 8 && count == 3 && !remaining &&
                   memcmp(data, "mwm", 3) == 0;
    XFree(data);
    return running;
}

bool mwm_run(char **argv, char **output, char **message)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *out = NULL, *err = NULL;
    int status;
    if (!g_spawn_sync(NULL, argv, NULL, 0, NULL, NULL, &out, &err, &status, &error)) {
        *message = g_strdup(error->message);
        return false;
    }
    if (!g_spawn_check_wait_status(status, &error)) {
        const char *detail = *g_strchomp(err) ? err : *g_strchomp(out) ? out : error->message;
        *message = g_utf8_make_valid(detail, -1);
        return false;
    }
    *output = g_steal_pointer(&out);
    return true;
}

void mwm_add_command(GPtrArray *items, const char *client, const char *label,
                     const char *kind, const char *const *args)
{
    g_autofree char *command = g_strjoinv(" ", (char **)args);
    g_autofree char *keywords = g_strconcat(kind, " ", command, NULL);
    Item *item = item_add(items, label, NULL, kind, keywords, ITEM_MWM_COMMAND);
    size_t count = g_strv_length((char **)args);
    item->argv = g_new0(char *, count + 2);
    item->argv[0] = g_strdup(client);
    for (size_t i = 0; i < count; i++)
        item->argv[i + 1] = g_strdup(args[i]);
}

static void add_tags(GPtrArray *items, const char *client, char *output)
{
    g_auto(GStrv) lines = g_strsplit(output, "\n", 0);
    for (size_t i = 0; lines[i]; i++) {
        char *tag = lines[i];
        if (!g_str_has_prefix(tag, "tag ") || !g_ascii_isdigit(tag[4]))
            continue;
        char *name;
        gint64 index = g_ascii_strtoll(tag + 4, &name, 10);
        char *fields = g_strrstr(name, " mon=");
        if (*name != ' ' || !fields || fields <= name + 1 || index >= G_MAXINT)
            continue;
        *fields = '\0';
        name++;
        // The query uses zero-based indexes. Numeric command selectors are one-based.
        g_autofree char *id = g_strdup_printf("%d", (int)index + 1);
        g_autofree char *focus = g_strdup_printf("Focus tag %s", name);
        g_autofree char *move = g_strdup_printf("Move window to tag %s", name);
        mwm_add_command(items, client, focus, "Tag", (const char *[]){"focus", "tag", id, NULL});
        mwm_add_command(items, client, move, "Tag", (const char *[]){"move", "win", id, NULL});
    }
}

static void add_monitors(GPtrArray *items, const char *client, char *output)
{
    g_auto(GStrv) lines = g_strsplit(output, "\n", 0);
    if (!*lines[0] || !lines[1] || !*lines[1])
        return;
    for (size_t i = 0; lines[i]; i++) {
        char id[12], geometry[64];
        if (sscanf(lines[i], "mon %11[0-9] %63s", id, geometry) != 2)
            continue;
        g_autofree char *focus = g_strdup_printf("Focus monitor %s · %s", id, geometry);
        g_autofree char *move = g_strdup_printf("Move current tag to monitor %s · %s", id, geometry);
        mwm_add_command(items, client, focus, "Monitor", (const char *[]){"focus", "mon", id, NULL});
        mwm_add_command(items, client, move, "Monitor", (const char *[]){"move", "tag", id, NULL});
    }
    mwm_add_command(items, client, "Move window to next monitor", "Monitor",
                    (const char *[]){"move", "win", "mon", "next", NULL});
    mwm_add_command(items, client, "Move window to previous monitor", "Monitor",
                    (const char *[]){"move", "win", "mon", "prev", NULL});
}

bool mwm_load(GPtrArray *items, const char *client, char **error)
{
    g_autofree char *tags = NULL, *monitors = NULL;
    char *query_tags[] = {(char *)client, "query", "tags", NULL};
    char *query_monitors[] = {(char *)client, "query", "monitors", NULL};
    if (!mwm_run(query_tags, &tags, error) || !mwm_run(query_monitors, &monitors, error))
        return false;

    static const struct { const char *label, *kind, *args[4]; } commands[] = {
        {"Focus next window", "Window", {"focus", "next"}},
        {"Focus previous window", "Window", {"focus", "prev"}},
        {"Focus last-used window", "Window", {"focus", "last"}},
        {"Focus next tag", "Tag", {"focus", "tag", "next"}},
        {"Focus previous tag", "Tag", {"focus", "tag", "prev"}},
        {"Move window to next tag", "Tag", {"move", "win", "next"}},
        {"Move window to previous tag", "Tag", {"move", "win", "prev"}},
        {"Promote focused window to master", "Window", {"move", "win", "master"}},
        {"Swap window with next in stack", "Window", {"swap", "next"}},
        {"Swap window with previous in stack", "Window", {"swap", "prev"}},
        {"Columns layout", "Layout", {"layout", "cols"}},
        {"Master stack layout", "Layout", {"layout", "stack"}},
        {"Floating layout", "Layout", {"layout", "float"}},
        {"Next layout", "Layout", {"layout", "next"}},
        {"Previous layout", "Layout", {"layout", "prev"}},
        {"Increase master count", "Layout", {"master", "+1"}},
        {"Decrease master count", "Layout", {"master", "-1"}},
        {"Widen master area", "Layout", {"master", "ratio", "+0.05"}},
        {"Narrow master area", "Layout", {"master", "ratio", "-0.05"}},
        {"Increase window gaps", "Layout", {"gap", "+5"}},
        {"Decrease window gaps", "Layout", {"gap", "-5"}},
        {"Toggle floating window", "Window", {"win", "toggle", "float"}},
        {"Toggle fullscreen window", "Window", {"win", "toggle", "full"}},
        {"Toggle sticky window", "Window", {"win", "toggle", "sticky"}},
        {"Center focused window", "Window", {"win", "center"}},
        {"Toggle bar", "Desktop", {"bar", "toggle"}},
        {"Toggle system tray", "Desktop", {"traybar", "toggle"}},
        {"Reload mwm configuration", "Desktop", {"reload"}},
        {"Close focused window", "Window", {"win", "close"}},
    };
    add_tags(items, client, tags);
    add_monitors(items, client, monitors);
    for (size_t i = 0; i < G_N_ELEMENTS(commands); i++)
        mwm_add_command(items, client, commands[i].label, commands[i].kind, commands[i].args);
    return true;
}
