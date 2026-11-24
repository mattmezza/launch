#include <X11/Xresource.h>
#include <X11/keysym.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "launch.h"

static const char *const keys[COLORS] = {
    "background", "foreground", "muted", "selbackground", "selforeground",
    "backgroundOut", "foregroundOut", "border", "accent", "chartCpu", "chartGpu",
    "chartRam", "chartRead", "chartWrite", "chartRx", "chartTx"
};

static void update(char **target, const char *value)
{
    g_free(*target);
    *target = g_strdup(value);
}

void settings_init(Settings *s, bool compatible)
{
    static const char *const palette[COLORS] = {
        "#161722", "#f3f3fa", "#a7a9bd", "#30324b", "#ffffff",
        "#234b4e", "#d6ffff", "#393b51", "#c7bcff", "#ac9fff", "#f59ac4",
        "#7ce1c3", "#ffc486", "#f794af", "#70ddea", "#a7b5ff"
    };
    *s = (Settings){
        .compatible = compatible, .toggle = !compatible, .fuzzy = !compatible, .path_commands = true,
        .width = 672, .startup = FREQUENT, .corner_radius = compatible ? 0 : 4,
        .border_width = compatible ? 0 : 2,
        .border_glow = !compatible, .glow_search_only = true, .glow_speed = 180,
        .system_interval_ms = 1000,
        .files_fallback = !compatible, .files_cache_seconds = 300, .files_max_entries = 200000,
        .web_enabled = !compatible,
        .rows = compatible ? 0 : 7, .monitor = -1, .placement = compatible ? TOP : CENTER,
        .font = g_strdup(compatible ? "Geist 14" : "Geist 15"),
        .mono = g_strdup("Geist Mono 13"),
        .shortcut[KEY_TOGGLE] = {XK_space, Mod4Mask}, .file = g_key_file_new(),
    };
    for (int i = 0; i < COLORS; i++)
        s->color[i] = g_strdup(palette[i]);
}

char *expand_home(const char *path)
{
    if (strcmp(path, "~") == 0)
        return g_strdup(g_get_home_dir());
    return g_str_has_prefix(path, "~/") ? g_build_filename(g_get_home_dir(), path + 2, NULL)
                                        : g_strdup(path);
}

void settings_resources(Settings *s, Display *display)
{
    XrmInitialize();
    const char *resources = XResourceManagerString(display);
    if (!resources)
        return;

    XrmDatabase database = XrmGetStringDatabase(resources);
    if (!database)
        return;

    const char *const namespaces[] = {"dmenu", "launch"};
    for (size_t space = 0; space < G_N_ELEMENTS(namespaces); space++) {
        for (int i = 0; i < COLORS + 2; i++) {
            if (i >= CHART_CPU && i < COLORS && (space == 0 || s->compatible))
                continue;
            const char *key = i == COLORS ? "font" : i == COLORS + 1 ? "mono" : keys[i];
            char **target = i == COLORS ? &s->font : i == COLORS + 1 ? &s->mono : &s->color[i];
            char name[64];
            g_snprintf(name, sizeof name, "%s.%s", namespaces[space], key);
            char *type;
            XrmValue value;
            if (XrmGetResource(database, name, name, &type, &value) &&
                strcmp(type, "String") == 0 && value.addr)
                update(target, value.addr);
        }
    }
    XrmDestroyDatabase(database);
}

static bool number(const char *str, int minimum, int maximum, int *dest)
{
    char *end;
    errno = 0;
    long n = strtol(str, &end, 10);
    if (errno || end == str || *end || n < minimum || n > maximum)
        return false;
    *dest = (int)n;
    return true;
}

static bool config_bool(const Settings *s, const char *group, const char *key, bool *value)
{
    if (!g_key_file_has_key(s->file, group, key, NULL))
        return true;
    g_autoptr(GError) error = NULL;
    *value = g_key_file_get_boolean(s->file, group, key, &error);
    if (error)
        fprintf(stderr, "launch: [%s] %s\n", group, error->message);
    return !error;
}

static bool config_search(Settings *s)
{
    if (!config_bool(s, "files", "fallback", &s->files_fallback) ||
        !config_bool(s, "files", "show-hidden", &s->files_hidden) ||
        !config_bool(s, "web", "enabled", &s->web_enabled))
        return false;
    g_autofree char *age = g_key_file_get_string(s->file, "files", "cache-seconds", NULL);
    g_autofree char *limit = g_key_file_get_string(s->file, "files", "max-entries", NULL);
    if ((age && !number(age, 0, 86400, &s->files_cache_seconds)) ||
        (limit && !number(limit, 1, 1000000, &s->files_max_entries))) {
        fprintf(stderr, "launch: invalid [files] cache-seconds or max-entries\n");
        return false;
    }
    s->files_roots = g_key_file_get_string_list(s->file, "files", "roots", NULL, NULL);
    if (!s->files_roots) {
        s->files_roots = g_new0(char *, 4);
        const GUserDirectory locations[] = {G_USER_DIRECTORY_DOCUMENTS,
                                            G_USER_DIRECTORY_DOWNLOAD, G_USER_DIRECTORY_DESKTOP};
        const char *const fallback[] = {"Documents", "Downloads", "Desktop"};
        for (size_t i = 0; i < G_N_ELEMENTS(locations); i++) {
            const char *path = g_get_user_special_dir(locations[i]);
            s->files_roots[i] = path && strcmp(path, g_get_home_dir()) != 0
                ? g_strdup(path) : g_build_filename(g_get_home_dir(), fallback[i], NULL);
        }
    }
    for (size_t i = 0; s->files_roots[i]; i++) {
        g_autofree char *path = expand_home(g_strstrip(s->files_roots[i]));
        if (!g_path_is_absolute(path)) {
            fprintf(stderr, "launch: [files] roots must be absolute paths or start with ~/\n");
            return false;
        }
        g_free(s->files_roots[i]);
        s->files_roots[i] = g_steal_pointer(&path);
    }
    s->files_exclude = g_key_file_get_string_list(s->file, "files", "exclude", NULL, NULL);
    if (!s->files_exclude)
        s->files_exclude = g_strsplit(".git;.hg;.svn;node_modules;__pycache__;build;target;.venv", ";", -1);
    for (size_t i = 0; s->files_exclude[i]; i++)
        g_strstrip(s->files_exclude[i]);
    return true;
}

static bool config_group(Settings *s, const char *group)
{
    static const char *const fields[] = {
        "width", "rows", "row-height", "monitor", "offset-x", "offset-y", "corner-radius", "border-width"
    };
    int *const targets[] = {&s->width, &s->rows, &s->row_height, &s->monitor,
                           &s->offset_x, &s->offset_y, &s->corner_radius, &s->border_width};
    const int low[] = {1, 0, 0, -1, -16384, -16384, 0, 0};
    const int high[] = {16384, 1024, 512, INT_MAX, 16384, 16384, 256, 64};
    for (size_t i = 0; i < G_N_ELEMENTS(fields); i++) {
        g_autofree char *value = g_key_file_get_string(s->file, group, fields[i], NULL);
        if (value && !number(value, low[i], high[i], targets[i])) {
            fprintf(stderr, "launch: invalid [%s] %s\n", group, fields[i]);
            return false;
        }
    }

    if (!config_bool(s, group, "path-commands", &s->path_commands) ||
        (!s->compatible && !config_bool(s, group, "toggle", &s->toggle)))
        return false;

    g_autofree char *place = g_key_file_get_string(s->file, group, "placement", NULL);
    if (place) {
        if (strcmp(place, "top") == 0)
            s->placement = TOP;
        else if (strcmp(place, "bottom") == 0)
            s->placement = BOTTOM;
        else if (strcmp(place, "center") == 0)
            s->placement = CENTER;
        else {
            fprintf(stderr, "launch: invalid [%s] placement\n", group);
            return false;
        }
    }

    for (int i = 0; i < (s->compatible ? CHART_CPU : COLORS); i++) {
        g_autofree char *value = g_key_file_get_string(s->file, group, keys[i], NULL);
        if (value)
            update(&s->color[i], value);
    }
    g_autofree char *font = g_key_file_get_string(s->file, group, "font", NULL);
    g_autofree char *mono = g_key_file_get_string(s->file, group, "mono", NULL);
    if (font)
        update(&s->font, font);
    if (mono)
        update(&s->mono, mono);
    return true;
}

static bool config_launch(Settings *s)
{
    const char *group = "launch";
    if (!config_bool(s, group, "border-glow", &s->border_glow) ||
        !config_bool(s, group, "glow-search-only", &s->glow_search_only))
        return false;
    g_autofree char *speed = g_key_file_get_string(s->file, group, "glow-speed", NULL);
    if (speed && !number(speed, 1, 2000, &s->glow_speed)) {
        fprintf(stderr, "launch: invalid [launch] glow-speed\n");
        return false;
    }
    g_autofree char *interval = g_key_file_get_string(s->file, "system", "interval-ms", NULL);
    if (interval && !number(interval, SYSTEM_MIN_INTERVAL_MS, SYSTEM_MAX_INTERVAL_MS, &s->system_interval_ms)) {
        fprintf(stderr, "launch: invalid [system] interval-ms\n");
        return false;
    }
    g_autofree char *startup = g_key_file_get_string(s->file, group, "startup", NULL);
    if (!startup || strcmp(startup, "frequent") == 0)
        s->startup = FREQUENT;
    else if (strcmp(startup, "clock") == 0)
        s->startup = CLOCK;
    else if (strcmp(startup, "minimal") == 0)
        s->startup = MINIMAL;
    else {
        fprintf(stderr, "launch: invalid [launch] startup\n");
        return false;
    }
    return true;
}

static bool config_keys(Settings *s)
{
    static const char *const names[] = {"up", "down", "accept", "cancel", "toggle"};
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autofree char *binding = g_key_file_get_string(s->file, "keys", names[i], NULL);
        if (!binding)
            continue;

        g_auto(GStrv) parts = g_strsplit(binding, "+", 0);
        unsigned modifiers = 0;
        s->shortcut[i] = (Shortcut){0};
        for (size_t j = 0; parts[j]; j++) {
            const char *part = g_strstrip(parts[j]);
            if (!parts[j + 1]) {
                s->shortcut[i] = (Shortcut){XStringToKeysym(part), modifiers};
                break;
            }
            if (g_ascii_strcasecmp(part, "ctrl") == 0)
                modifiers |= ControlMask;
            else if (g_ascii_strcasecmp(part, "alt") == 0)
                modifiers |= Mod1Mask;
            else if (g_ascii_strcasecmp(part, "shift") == 0)
                modifiers |= ShiftMask;
            else if (g_ascii_strcasecmp(part, "super") == 0)
                modifiers |= Mod4Mask;
            else
                break;
        }
        if (!s->shortcut[i].symbol || (i == KEY_TOGGLE && !s->shortcut[i].modifiers)) {
            fprintf(stderr, "launch: invalid [keys] %s\n", names[i]);
            return false;
        }
    }
    return true;
}

bool settings_config(Settings *s)
{
    const char *home = g_getenv("XDG_CONFIG_HOME");
    g_autofree char *fallback = NULL;
    if (!home || !g_path_is_absolute(home)) {
        fallback = g_build_filename(g_get_home_dir(), ".config", NULL);
        home = fallback;
    }
    g_autofree char *path = g_build_filename(home, "launch", "config.ini", NULL);
    g_autoptr(GError) error = NULL;
    if (!g_key_file_load_from_file(s->file, path, G_KEY_FILE_NONE, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            fprintf(stderr, "launch: %s\n", error->message);
            return false;
        }
    }
    return config_group(s, "theme") && config_group(s, s->compatible ? "dmenu" : "launch") &&
           (s->compatible || (config_launch(s) && config_keys(s) && config_search(s)));
}

char **workflow_command(const Settings *s, const char *group)
{
    g_autofree char *command = g_key_file_get_string(s->file, group, "command", NULL);
    char **argv;
    if (command && g_shell_parse_argv(command, NULL, &argv, NULL))
        return argv;
    fprintf(stderr, "launch: [%s] requires a command with valid arguments\n", group);
    return NULL;
}

int workflow_run(const char *name, int argc, char **argv)
{
    Settings settings;
    settings_init(&settings, false);
    if (!settings_config(&settings)) {
        settings_clear(&settings);
        return 1;
    }

    g_auto(GStrv) groups = g_key_file_get_groups(settings.file, NULL);
    g_autofree char *needle = search_key(name);
    g_auto(GStrv) command = NULL;
    for (size_t i = 0; groups[i]; i++) {
        if (!g_str_has_prefix(groups[i], "workflow ") || !groups[i][9])
            continue;
        g_autofree char *candidate = search_key(groups[i] + 9);
        if (strcmp(needle, candidate) == 0) {
            command = workflow_command(&settings, groups[i]);
            break;
        }
    }
    settings_clear(&settings);
    if (!command) {
        fprintf(stderr, "launch: workflow unavailable: %s\n", name);
        return 1;
    }

    size_t count = g_strv_length(command);
    g_autofree char **args = g_new0(char *, count + (size_t)argc + 1);
    memcpy(args, command, count * sizeof *args);
    memcpy(args + count, argv, (size_t)argc * sizeof *args);
    execvp(args[0], args);
    fprintf(stderr, "launch: %s: %s\n", args[0], strerror(errno));
    return 1;
}

void usage(FILE *stream, bool compatible)
{
    if (compatible)
        fprintf(stream, "usage: dmenu [-bfiv] [-l lines] [-m monitor] [-p prompt]\n"
                        "             [-fn font] [-nb color] [-nf color] [-sb color] [-sf color]\n"
                        "             [-ob color] [-of color] [-w window-id]\n");
    else
        fprintf(stream, "usage: launch [pick | emoji | calc | system | calendar | files] [options]\n"
                        "       launch workflow name [arguments...]\n"
                        "Options: -p prompt, -l rows, -m monitor, -b (bottom), -i, -fn font\n"
                        "         --print (emoji or calc), --literal / --require-match (pick)\n"
                        "         --help, --version\n");
}

bool settings_args(Settings *s, int argc, char **argv, Mode *mode)
{
    int start = 1;
    *mode = s->compatible ? PICK : PALETTE;
    if (!s->compatible && argc > 1) {
        if (strcmp(argv[1], "pick") == 0)
            *mode = PICK;
        else if (strcmp(argv[1], "emoji") == 0)
            *mode = EMOJI;
        else if (strcmp(argv[1], "calc") == 0)
            *mode = CALCULATOR;
        else if (strcmp(argv[1], "system") == 0)
            *mode = SYSTEM;
        else if (strcmp(argv[1], "calendar") == 0)
            *mode = CALENDAR;
        else if (strcmp(argv[1], "files") == 0)
            *mode = FILES;
        if (*mode != PALETTE)
            start = 2;
    }
    static const struct { const char *flag; Color color; } flags[] = {
        {"-nb", BG}, {"-nf", FG}, {"-sb", SELECT_BG}, {"-sf", SELECT_FG},
        {"-ob", OUT_BG}, {"-of", OUT_FG}
    };
    for (int i = start; i < argc; i++) {
        const char *flag = argv[i];
        if (strcmp(flag, "-b") == 0)
            s->placement = BOTTOM;
        else if (strcmp(flag, "-f") == 0)
            s->fast = true;
        else if (strcmp(flag, "-i") == 0)
            s->insensitive = true;
        else if (!s->compatible && strcmp(flag, "--print") == 0 &&
                 (*mode == EMOJI || *mode == CALCULATOR))
            s->print = true;
        else if (!s->compatible && strcmp(flag, "--literal") == 0 && *mode == PICK)
            s->fuzzy = false;
        else if (!s->compatible && strcmp(flag, "--require-match") == 0 && *mode == PICK)
            s->require_match = true;
        else {
            if (++i >= argc)
                return false;
            const char *value = argv[i];
            if (strcmp(flag, "-p") == 0)
                update(&s->prompt, value);
            else if (strcmp(flag, "-fn") == 0)
                update(&s->font, value);
            else if (strcmp(flag, "-l") == 0) {
                if (!number(value, 0, 16384, &s->rows))
                    return false;
            } else if (strcmp(flag, "-m") == 0) {
                if (!number(value, -1, INT_MAX, &s->monitor))
                    return false;
            } else if (strcmp(flag, "-w") == 0) {
                char *end;
                errno = 0;
                s->parent = strtoul(value, &end, 0);
                if (errno || *end || end == value || *value == '-' || s->parent > UINT32_MAX)
                    return false;
            } else {
                size_t j = 0;
                while (j < G_N_ELEMENTS(flags) && strcmp(flag, flags[j].flag))
                    j++;
                if (j == G_N_ELEMENTS(flags))
                    return false;
                update(&s->color[flags[j].color], value);
            }
        }
    }
    return true;
}

void settings_clear(Settings *s)
{
    g_key_file_free(s->file);
    g_free(s->prompt);
    g_free(s->font);
    g_free(s->mono);
    g_strfreev(s->files_roots);
    g_strfreev(s->files_exclude);
    for (int i = 0; i < COLORS; i++)
        g_free(s->color[i]);
}
