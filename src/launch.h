#ifndef LAUNCH_H
#define LAUNCH_H

#include <X11/Xlib.h>
#include <stdio.h>
#include <gio/gio.h>
#include "cui.h"
#include "system.h"
#include "files.h"
#include "rates.h"

enum { QUERY_CAPACITY = 4096 };
typedef enum { PALETTE, PICK, EMOJI, CALCULATOR, SYSTEM, CALENDAR, FILES, MWM } Mode;
typedef enum { FREQUENT, CLOCK, MINIMAL } Startup;
typedef enum { CENTER, TOP, BOTTOM } Placement;
typedef enum { KEY_UP, KEY_DOWN, KEY_ACCEPT, KEY_CANCEL, KEY_TOGGLE, KEY_ACTIONS } KeyAction;
typedef struct {
    KeySym symbol;
    unsigned modifiers;
} Shortcut;
typedef enum { ITEM_OUTPUT, ITEM_APP, ITEM_EXECUTABLE, ITEM_COMMAND,
               ITEM_EMOJI, ITEM_CALCULATOR, ITEM_SYSTEM, ITEM_CALENDAR, ITEM_FILES,
               ITEM_FILE, ITEM_FOLDER, ITEM_WEB, ITEM_MWM, ITEM_MWM_COMMAND, ITEM_CONVERSION } Action;
typedef enum { BG, FG, MUTED, SELECT_BG, SELECT_FG, OUT_BG, OUT_FG, BORDER, ACCENT,
               CHART_CPU, CHART_GPU, CHART_RAM, CHART_READ, CHART_WRITE, CHART_RX, CHART_TX, COLORS } Color;

typedef struct {
    bool compatible, insensitive, fast, print, fuzzy, path_commands, require_match, toggle;
    bool border_glow, glow_search_only;
    bool files_fallback, files_hidden, web_enabled;
    int width, rows, row_height, monitor, offset_x, offset_y;
    int border_width, corner_radius, glow_speed, system_interval_ms;
    int files_cache_seconds, files_max_entries;
    Placement placement;
    Startup startup;
    Window parent;
    char *prompt, *font, *mono, *color[COLORS];
    char **files_roots, **files_exclude, **files_reveal;
    Shortcut shortcut[KEY_ACTIONS];
    GKeyFile *file;
} Settings;

typedef struct {
    char *label, *value, *note, *detail;
    char *key, *aliases, *usage_id;
    Action action;
    GAppInfo *app;
    GSocket *browser_socket;
    char **argv;
    int launches;
    gint64 last_used;
    bool emitted, direct_path;
} Item;

typedef struct {
    size_t index;
    int score;
    gint64 last_used;
} Match;

typedef struct {
    Mode mode;
    GPtrArray *palette, *items, *emoji, *file_items, *web_items, *mwm_items, *mwm_input;
    GPtrArray *conversion_items;
    FileSearch *files;
    Rates *rates;
    GHashTable *bangs;
    GArray *matches;
    size_t selected, first, page, columns;
    size_t cursor, anchor, saved_cursor, saved_anchor, saved_selection, saved_first;
    char query[QUERY_CAPACITY], saved_query[QUERY_CAPACITY];
    char result[64];
    char *output, *error, *file_query, *mwm_client;
    int file_request;
    Item *chosen;
    bool came_from_palette, done, accepted, dirty, file_pending, converting;
} Model;

void settings_init(Settings *, bool);
bool settings_args(Settings *, int, char **, Mode *);
void settings_resources(Settings *, Display *);
bool settings_config(Settings *);
void settings_clear(Settings *);
char **workflow_command(const Settings *, const char *group);
int workflow_run(const char *name, int argc, char **argv);

void model_init(Model *, Mode);
bool model_load(Model *, const Settings *, Display *);
void model_clear(Model *);
Item *item_add(GPtrArray *, const char *label, const char *value, const char *note,
               const char *aliases, Action);
void model_filter(Model *, const Settings *);
Item *model_item(const Model *, size_t rank);
bool model_selection(const Model *, size_t *from, size_t *to);
void model_seek(Model *, size_t, bool extend);
void model_insert(Model *, const char *, size_t);
void model_erase(Model *, size_t from, size_t to);
void model_select(Model *, size_t);
bool model_reveal(Model *, const Settings *);
bool model_accept(Model *, const Settings *, Display *, bool raw, bool keep);
bool model_back(Model *, const Settings *);
bool model_dispatch(const Model *, const Settings *);
bool model_showcase(const Model *, const Settings *);
bool mode_has_search(Mode);
bool model_file_update(Model *);
bool model_rates_update(Model *, const Settings *);
void usage_load(GPtrArray *);
void usage_record(const Item *);
char *search_key(const char *text);
int search_score(const char *key, const char *aliases, const char *query, char **tokens,
                 bool strict, bool fuzzy_aliases);
char *data_contents(const char *name);
char *expand_home(const char *path);
bool calculate(const char *, char *, size_t);
bool convert(Model *);

int clipboard_serve(int, const char *);
bool clipboard_copy(const char *);
int x11_run(Settings *, Mode, int argc, char **argv);
void usage(FILE *, bool);

#endif
