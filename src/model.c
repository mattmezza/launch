#include <unistd.h>
#include <gio/gdesktopappinfo.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "launch.h"
#include "web.h"

#ifndef LAUNCH_DATA_DIR
#define LAUNCH_DATA_DIR "/usr/local/share/launch"
#endif

static char *valid_text(const char *str)
{
    return g_utf8_validate(str, -1, NULL) ? g_strdup(str) : g_utf8_make_valid(str, -1);
}

char *search_key(const char *text)
{
    if (g_str_is_ascii(text))
        return g_ascii_strdown(text, -1);
    g_autofree char *valid = valid_text(text);
    g_autofree char *folded = g_utf8_casefold(valid, -1);
    g_autofree char *normalized = g_utf8_normalize(folded, -1, G_NORMALIZE_NFKD);
    GString *key = g_string_sized_new(strlen(normalized));
    for (const char *p = normalized; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (!g_unichar_ismark(c))
            g_string_append_unichar(key, c);
    }
    return g_string_free(key, FALSE);
}

static Item *add_item(GPtrArray *list, const char *label, const char *value,
                      const char *note, const char *aliases, Action action)
{
    Item *item = g_new0(Item, 1);
    item->label = valid_text(label);
    item->value = g_strdup(value);
    item->note = g_strdup(note);
    item->aliases = g_strdup(aliases);
    item->action = action;
    g_ptr_array_add(list, item);
    return item;
}

static void item_free(gpointer data)
{
    Item *item = data;
    g_free(item->label);
    g_free(item->value);
    g_free(item->note);
    g_free(item->detail);
    g_free(item->key);
    g_free(item->aliases);
    g_free(item->usage_id);
    g_clear_object(&item->app);
    g_clear_object(&item->browser_socket);
    g_strfreev(item->argv);
    g_free(item);
}

void model_init(Model *m, Mode mode)
{
    *m = (Model){.mode = mode, .columns = 1, .anchor = SIZE_MAX, .dirty = true};
    m->palette = g_ptr_array_new_with_free_func(item_free);
    m->matches = g_array_new(FALSE, FALSE, sizeof(Match));
    m->items = m->palette;
}

static gint compare_apps(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(g_app_info_get_display_name(G_APP_INFO((gpointer)a)),
                          g_app_info_get_display_name(G_APP_INFO((gpointer)b)));
}

static void load_commands(Model *m, GHashTable *seen)
{
    const char *path = g_getenv("PATH");
    if (!path)
        return;
    g_auto(GStrv) dirs = g_strsplit(path, G_SEARCHPATH_SEPARATOR_S, 0);
    for (size_t i = 0; dirs[i]; i++) {
        const char *dir = *dirs[i] ? dirs[i] : ".";
        GDir *listing = g_dir_open(dir, 0, NULL);
        if (!listing)
            continue;
        const char *name;
        while ((name = g_dir_read_name(listing))) {
            if (*name == '.' || g_hash_table_contains(seen, name))
                continue;
            g_autofree char *relative = g_build_filename(dir, name, NULL);
            g_autofree char *file = g_canonicalize_filename(relative, NULL);
            if (access(file, X_OK) != 0 || !g_file_test(file, G_FILE_TEST_IS_REGULAR))
                continue;
            g_hash_table_add(seen, g_strdup(name));
            Item *item = add_item(m->palette, name, NULL, "Command", NULL, ITEM_EXECUTABLE);
            item->argv = g_new0(char *, 2);
            item->argv[0] = g_steal_pointer(&file);
        }
        g_dir_close(listing);
    }
}

static Item *add_workflow(Model *m, const char *name, const char *value,
                          const char *keywords, Action action)
{
    Item *item = add_item(m->palette, name, value, "Workflow", keywords, action);
    g_autofree char *key = search_key(name);
    item->usage_id = g_strconcat("workflow:", key, NULL);
    return item;
}

static void load_palette(Model *m, const Settings *s)
{
    add_workflow(m, "Emoji", "emoji", "emoticon symbols smileys", ITEM_EMOJI);
    add_workflow(m, "Calculator", "calc", "math calc arithmetic evaluate", ITEM_CALCULATOR);
    add_workflow(m, "System", "system", "monitor stats performance cpu gpu ram memory disk network", ITEM_SYSTEM);
    add_workflow(m, "Calendar", "calendar", "date today day week month year agenda", ITEM_CALENDAR);
    add_workflow(m, "Files", "files", "find file folder document path", ITEM_FILES);
    g_auto(GStrv) groups = g_key_file_get_groups(s->file, NULL);
    for (size_t i = 0; groups[i]; i++) {
        if (!g_str_has_prefix(groups[i], "workflow ") || !groups[i][9])
            continue;
        char **argv = workflow_command(s, groups[i]);
        if (!argv)
            continue;
        g_autofree char *keywords = g_key_file_get_string(s->file, groups[i], "keywords", NULL);
        Item *item = add_item(m->palette, groups[i] + 9, NULL, "Workflow", keywords, ITEM_COMMAND);
        item->usage_id = g_strconcat("script:", groups[i] + 9, NULL);
        item->argv = argv;
    }

    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GList *apps = g_list_sort(g_app_info_get_all(), compare_apps);
    for (GList *link = apps; link; link = link->next) {
        GAppInfo *app = link->data;
        if (!g_app_info_should_show(app))
            continue;
        const char *const *keywords = G_IS_DESKTOP_APP_INFO(app)
            ? g_desktop_app_info_get_keywords(G_DESKTOP_APP_INFO(app)) : NULL;
        g_autofree char *tags = keywords ? g_strjoinv(" ", (char **)keywords) : NULL;
        const char *executable = g_app_info_get_executable(app);
        if (executable && *executable)
            g_hash_table_add(seen, g_path_get_basename(executable));
        g_autofree char *aliases = g_strconcat(executable ? executable : "", " ",
                                               tags ? tags : "", NULL);
        Item *item = add_item(m->palette, g_app_info_get_display_name(app), NULL,
                              "App", aliases, ITEM_APP);
        item->app = g_object_ref(app);
        const char *id = g_app_info_get_id(app);
        if (id)
            item->usage_id = g_strconcat("app:", id, NULL);
    }
    g_list_free_full(apps, g_object_unref);
    if (s->path_commands)
        load_commands(m, seen);
    if (s->startup == FREQUENT)
        usage_load(m->palette);
}

char *data_contents(const char *name)
{
#ifdef LAUNCH_STATIC_RELEASE
    extern const char launch_config_data[], launch_emoji_data[];
    if (strcmp(name, "config.ini") == 0)
        return g_strdup(launch_config_data);
    if (strcmp(name, "emoji.tsv") == 0)
        return g_strdup(launch_emoji_data);
#endif
    char *contents = NULL;
    g_autofree char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe) {
        g_autofree char *dir = g_path_get_dirname(exe);
        g_autofree char *source = g_build_filename(dir, "..", "assets", name, NULL);
        g_autofree char *reference = g_build_filename(dir, "..", name, NULL);
        g_autofree char *installed = g_build_filename(dir, "..", "share", "launch", name, NULL);
        if (g_file_get_contents(source, &contents, NULL, NULL) ||
            g_file_get_contents(installed, &contents, NULL, NULL) ||
            (strcmp(name, "config.ini") == 0 && g_file_get_contents(reference, &contents, NULL, NULL)))
            return contents;
    }
    g_autofree char *data = g_build_filename(LAUNCH_DATA_DIR, name, NULL);
    g_file_get_contents(data, &contents, NULL, NULL);
    return contents;
}

static bool load_emoji(Model *m)
{
    if (m->emoji)
        return true;
    g_autofree char *contents = data_contents("emoji.tsv");
    if (!contents) {
        g_free(m->error);
        m->error = g_strdup("Emoji data file unavailable");
        return false;
    }

    m->emoji = g_ptr_array_new_with_free_func(item_free);
    g_auto(GStrv) lines = g_strsplit(contents, "\n", 0);
    for (size_t i = 0; lines[i]; i++) {
        g_auto(GStrv) fields = g_strsplit(lines[i], "\t", 3);
        if (fields[0] && *fields[0] && fields[1] && *fields[1]) {
            g_autofree char *aliases = g_strconcat(fields[0], " ", fields[2] ? fields[2] : "", NULL);
            add_item(m->emoji, fields[1], fields[0], NULL, aliases, ITEM_OUTPUT);
        }
    }
    return true;
}

bool model_load(Model *m, const Settings *s)
{
    if (m->mode == EMOJI) {
        if (!load_emoji(m))
            return false;
        m->items = m->emoji;
    } else if (m->mode == PICK) {
        size_t capacity = 0;
        char *line = NULL;
        ssize_t length;
        while ((length = getline(&line, &capacity, stdin)) >= 0) {
            if (length && line[length - 1] == '\n')
                line[length - 1] = '\0';
            add_item(m->items, line, line, NULL, NULL, ITEM_OUTPUT);
        }
        free(line);
        if (ferror(stdin)) {
            m->error = g_strdup("Could not read items from stdin");
            return false;
        }
    } else if (m->mode == PALETTE) {
        load_palette(m, s);
    }
    return true;
}

void model_clear(Model *m)
{
    files_close(m->files);
    g_ptr_array_unref(m->palette);
    g_clear_pointer(&m->emoji, g_ptr_array_unref);
    g_clear_pointer(&m->file_items, g_ptr_array_unref);
    g_clear_pointer(&m->web_items, g_ptr_array_unref);
    g_clear_pointer(&m->bangs, g_hash_table_unref);
    g_array_unref(m->matches);
    g_free(m->output);
    g_free(m->error);
    g_free(m->file_query);
}

static bool word_boundary(const char *start, const char *pos)
{
    return start == pos || !g_unichar_isalnum(g_utf8_get_char(g_utf8_prev_char(pos)));
}

static int contiguous_score(const char *field, const char *token)
{
    const char *match = strstr(field, token);
    if (!match)
        return 0;
    if (match == field)
        return field[strlen(token)] ? 1150 : 1400;
    do {
        if (word_boundary(field, match))
            return 950;
        match = strstr(g_utf8_next_char(match), token);
    } while (match);
    return 700;
}

/* Prefer compact subsequences, especially those that start a word. */
static int subsequence_score(const char *field, const char *token, bool within_word)
{
    size_t length = (size_t)g_utf8_strlen(token, -1);
    if (length <= 1)
        return 0;
    const gunichar initial = g_utf8_get_char(token);
    int best = 0;
    for (const char *start = field; *start && start - field < 256;
         start = g_utf8_next_char(start)) {
        if (g_utf8_get_char(start) != initial)
            continue;
        const char *needle = g_utf8_next_char(token);
        int gap = 0;
        size_t skipped = 0, budget = within_word ? 2 : MAX(4, length + 2);
        int score = 355 + (word_boundary(field, start) ? 65 : 0);
        for (const char *p = g_utf8_next_char(start); *p && *needle && skipped <= budget;
             p = g_utf8_next_char(p)) {
            gunichar c = g_utf8_get_char(p);
            if (within_word && !g_unichar_isalnum(c))
                break;
            if (c == g_utf8_get_char(needle)) {
                score += (gap ? 4 : 18) + (word_boundary(field, p) ? 12 : 0);
                needle = g_utf8_next_char(needle);
                gap = 0;
            } else {
                score -= 13;
                gap++;
                skipped++;
            }
        }
        if (!*needle)
            best = MAX(best, CLAMP(score, 1, 649));
    }
    return best;
}

int search_score(const char *key, const char *aliases, const char *query, char **tokens,
                 bool strict, bool fuzzy_aliases)
{
    int total = 0;
    for (size_t i = 0; tokens[i]; i++) {
        const char *token = tokens[i];
        if (!*token)
            continue;
        int label = contiguous_score(key, token);
        int alias = contiguous_score(aliases, token);
        int score;
        if (fuzzy_aliases) {
            if (!label)
                label = subsequence_score(key, token, false);
            if (!alias)
                alias = subsequence_score(aliases, token, false);
            score = label ? label + 1000 : alias;
        } else {
            score = MAX(label, alias ? alias - 210 : 0);
            if (!score && !strict)
                score = subsequence_score(key, token, tokens[1] != NULL);
        }
        if (!score)
            return -1;
        total += score;
    }
    return total + (g_str_has_prefix(key, query) ? 150 : 0);
}

static int fuzzy_score(Item *item, const char *query, char **tokens, bool strict)
{
    if (!item->key) {
        item->key = search_key(item->label);
        char *aliases = search_key(item->aliases ? item->aliases : "");
        g_free(item->aliases);
        item->aliases = aliases;
    }
    if (strcmp(item->key, query) == 0 || (item->value && strcmp(item->value, query) == 0))
        return 1000000;
    return search_score(item->key, item->aliases, query, tokens, strict, false);
}

static int literal_score(const Item *item, const char *query, char **tokens, bool insensitive)
{
    const char *label = item->value;
    const char *first = NULL;
    for (size_t i = 0; tokens[i]; i++) {
        const char *token = tokens[i];
        if (!*token)
            continue;
        if (!first)
            first = token;
        if (!(insensitive ? strcasestr(label, token) : strstr(label, token)))
            return -1;
    }
    if (!first || (insensitive ? g_ascii_strcasecmp(label, query) : strcmp(label, query)) == 0)
        return 2;
    return (insensitive ? g_ascii_strncasecmp(label, first, strlen(first))
                        : strncmp(label, first, strlen(first))) == 0;
}

static gint compare_matches(gconstpointer left, gconstpointer right)
{
    const Match *a = left, *b = right;
    if (a->score != b->score)
        return a->score > b->score ? -1 : 1;
    if (a->last_used != b->last_used)
        return a->last_used > b->last_used ? -1 : 1;
    return (a->index > b->index) - (a->index < b->index);
}

bool model_showcase(const Model *m, const Settings *s)
{
    return m->mode == PALETTE && !*m->query && s->startup != FREQUENT;
}

bool mode_has_search(Mode mode)
{
    return mode != SYSTEM && mode != CALENDAR;
}

static void list_matches(Model *m)
{
    g_array_set_size(m->matches, 0);
    for (guint i = 0; i < m->items->len; i++) {
        Match match = {.index = i};
        g_array_append_val(m->matches, match);
    }
}

static void file_filter(Model *m, const Settings *s, const char *text)
{
    g_autofree char *trimmed = text ? g_strdup(text) : NULL;
    g_autofree char *path = trimmed && *g_strstrip(trimmed) ? expand_home(trimmed) : NULL;
    g_autofree char *query = path ? search_key(path) : NULL;
    if (g_strcmp0(query, m->file_query) != 0) {
        if (!m->files && query)
            m->files = files_create(s->files_roots, s->files_exclude, s->files_hidden,
                                    s->files_cache_seconds, s->files_max_entries);
        g_free(m->file_query);
        m->file_query = g_steal_pointer(&query);
        m->file_pending = m->files && m->file_query;
        if (m->files)
            m->file_request = files_request(m->files, m->file_query);
        if (m->file_items)
            g_ptr_array_set_size(m->file_items, 0);
    }
    if (!m->file_query)
        return;
    if (!m->files) {
        m->error = g_strdup("Could not start file search");
        return;
    }
    if (!m->file_items)
        m->file_items = g_ptr_array_new_with_free_func(item_free);
    m->items = m->file_items;
    list_matches(m);
}

bool model_file_update(Model *m)
{
    if (!m->files)
        return false;
    bool updated = false;
    FileResults *result;
    while ((result = files_receive(m->files))) {
        if (m->file_request == result->request && m->items == m->file_items) {
            const Item *selected = model_item(m, m->selected);
            g_autofree char *current = selected ? g_strdup(selected->value) : NULL;
            size_t rank = 0;
            g_ptr_array_set_size(m->file_items, 0);
            for (guint i = 0; i < result->matches->len; i++) {
                const FileMatch *match = g_ptr_array_index(result->matches, i);
                if (current && strcmp(current, match->path) == 0)
                    rank = i;
                g_autofree char *name = g_filename_display_basename(match->path);
                g_autofree char *parent = g_path_get_dirname(match->path);
                const char *home = g_get_home_dir();
                size_t home_length = strlen(home);
                g_autofree char *short_parent = g_str_has_prefix(parent, home) &&
                    (!parent[home_length] || parent[home_length] == '/')
                    ? g_strconcat("~", parent + home_length, NULL) : NULL;
                Item *item = add_item(m->file_items, name, match->path,
                                      match->directory ? "Folder" : "File", NULL,
                                      match->directory ? ITEM_FOLDER : ITEM_FILE);
                item->detail = g_filename_display_name(short_parent ? short_parent : parent);
            }
            m->file_pending = result->pending;
            list_matches(m);
            m->selected = m->first = 0;
            model_select(m, rank);
            updated = true;
        }
        files_results_free(result);
    }
    return updated;
}

static bool filter_web(Model *m, const Settings *s, const char *query)
{
    WebTarget target = {0};
    if (!s->web_enabled || !web_target(&m->bangs, s, query, &target))
        return false;
    if (target.error) {
        m->error = g_steal_pointer(&target.error);
    } else {
        if (!m->web_items)
            m->web_items = g_ptr_array_new_with_free_func(item_free);
        g_ptr_array_set_size(m->web_items, 0);
        g_autofree char *note = g_strconcat("@", target.profile, NULL);
        Item *item = add_item(m->web_items, target.label, target.url, note, NULL, ITEM_WEB);
        item->detail = g_steal_pointer(&target.detail);
        m->items = m->web_items;
        list_matches(m);
    }
    web_target_clear(&target);
    file_filter(m, s, NULL);
    return true;
}

void model_filter(Model *m, const Settings *s)
{
    if (!m->dirty)
        return;
    m->dirty = false;
    m->selected = m->first = 0;
    g_clear_pointer(&m->error, g_free);
    m->items = m->mode == EMOJI ? m->emoji : m->palette;
    g_array_set_size(m->matches, 0);
    if (!mode_has_search(m->mode) || model_showcase(m, s)) {
        file_filter(m, s, NULL);
        return;
    }
    if (m->mode == FILES) {
        file_filter(m, s, m->query);
        return;
    }
    if (m->mode == CALCULATOR) {
        m->result[0] = '\0';
        calculate(m->query, m->result, sizeof m->result);
        return;
    }

    if (m->mode == PALETTE) {
        g_autofree char *input = g_strdup(m->query);
        const char *text = g_strstrip(input);
        if (g_ascii_strncasecmp(text, "files", 5) == 0 && g_ascii_isspace(text[5])) {
            file_filter(m, s, text + 6);
            return;
        }
        if (filter_web(m, s, text))
            return;
    }
    g_autofree char *query = s->fuzzy ? search_key(m->query) : g_strdup(m->query);
    g_auto(GStrv) tokens = g_strsplit(query, " ", 0);
    bool strict = *query && m->mode == EMOJI && tokens[1] != NULL;
    for (size_t i = 0; i < m->items->len; i++) {
        Item *item = g_ptr_array_index(m->items, i);
        if (!*query && item->action == ITEM_EXECUTABLE)
            continue;
        int score = !*query ? item->launches : s->fuzzy ? fuzzy_score(item, query, tokens, strict)
                                           : literal_score(item, query, tokens, s->insensitive);
        if (score >= 0) {
            if (item->action == ITEM_EXECUTABLE)
                score--;
            Match match = {i, score, *query ? 0 : item->last_used};
            g_array_append_val(m->matches, match);
        }
    }
    if (*query || m->mode == PALETTE)
        g_array_sort(m->matches, compare_matches);
    if (m->mode == PALETTE)
        file_filter(m, s, s->files_fallback && !m->matches->len ? m->query : NULL);
}

Item *model_item(const Model *m, size_t rank)
{
    if (rank >= m->matches->len)
        return NULL;
    return g_ptr_array_index(m->items, g_array_index(m->matches, Match, rank).index);
}

bool model_selection(const Model *m, size_t *from, size_t *to)
{
    if (m->anchor == SIZE_MAX || m->cursor == m->anchor)
        return false;
    *from = MIN(m->cursor, m->anchor);
    *to = MAX(m->cursor, m->anchor);
    return true;
}

void model_seek(Model *m, size_t target, bool extend)
{
    if (!extend)
        m->anchor = SIZE_MAX;
    else if (m->anchor == SIZE_MAX)
        m->anchor = m->cursor;
    m->cursor = target;
}

void model_insert(Model *m, const char *text, size_t bytes)
{
    size_t used = strlen(m->query);
    size_t from = m->cursor, to = m->cursor;
    model_selection(m, &from, &to);
    if (!bytes || bytes >= sizeof m->query - used + (to - from) ||
        !g_utf8_validate(text, (gssize)bytes, NULL))
        return;
    memmove(m->query + from + bytes, m->query + to, used - to + 1);
    memcpy(m->query + from, text, bytes);
    model_seek(m, from + bytes, false);
    m->dirty = true;
}

void model_erase(Model *m, size_t from, size_t to)
{
    model_selection(m, &from, &to);
    if (from >= to)
        return;
    memmove(m->query + from, m->query + to, strlen(m->query + to) + 1);
    model_seek(m, from, false);
    m->dirty = true;
}

void model_select(Model *m, size_t rank)
{
    if (!m->matches->len)
        return;
    m->selected = MIN(rank, m->matches->len - 1);
    if (m->selected < m->first)
        m->first = (m->selected / m->columns) * m->columns;
    else if (m->page && m->selected - m->first >= m->page)
        m->first = (m->selected / m->columns + 1) * m->columns - m->page;
}

static bool enter_workflow(Model *m, const Settings *s, Mode mode)
{
    if (mode == EMOJI && !load_emoji(m))
        return false;
    memcpy(m->saved_query, m->query, sizeof m->query);
    m->saved_cursor = m->cursor;
    m->saved_anchor = m->anchor;
    m->saved_selection = m->selected;
    m->saved_first = m->first;
    m->came_from_palette = true;
    m->mode = mode;
    m->items = mode == EMOJI ? m->emoji : m->palette;
    m->query[0] = '\0';
    model_seek(m, 0, false);
    m->dirty = true;
    model_filter(m, s);
    return true;
}

bool model_back(Model *m, const Settings *s)
{
    if (!m->came_from_palette)
        return false;
    m->came_from_palette = false;
    m->mode = PALETTE;
    m->items = m->palette;
    memcpy(m->query, m->saved_query, sizeof m->query);
    m->cursor = m->saved_cursor;
    m->anchor = m->saved_anchor;
    m->dirty = true;
    model_filter(m, s);
    m->first = m->saved_first;
    m->selected = m->saved_selection;
    g_clear_pointer(&m->error, g_free);
    return true;
}

bool model_accept(Model *m, const Settings *s, Display *display, bool raw, bool keep)
{
    Item *item = model_item(m, m->selected);
    if (m->mode == PICK) {
        if (s->require_match && (!item || raw))
            return true;
        puts(item && !raw ? item->value : m->query);
        if (fflush(stdout) == EOF)
            return false;
        if (keep) {
            if (item)
                item->emitted = true;
            return true;
        }
    } else if (m->mode == EMOJI || m->mode == CALCULATOR) {
        const char *value = m->mode == EMOJI ? (item ? item->value : NULL) : m->result;
        if (!value || !*value)
            return true;
        if (s->print) {
            puts(value);
            if (fflush(stdout) == EOF)
                return false;
        } else {
            m->output = g_strdup(value);
        }
    } else if (!item) {
        return true;
    } else if (item->action == ITEM_FILE || item->action == ITEM_FOLDER) {
        struct stat info;
        g_clear_pointer(&m->error, g_free);
        if (keep)
            m->output = g_strdup(item->value);
        else if (lstat(item->value, &info) < 0 ||
                 (item->action == ITEM_FOLDER ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode))) {
            m->error = g_strdup("This path is no longer available");
            return true;
        } else {
            item->argv = g_new0(char *, 3);
            item->argv[0] = g_strdup("xdg-open");
            item->argv[1] = g_strdup(item->value);
            m->chosen = item;
        }
    } else if (item->action == ITEM_WEB) {
        g_clear_pointer(&m->error, g_free);
        item->argv = web_command(s, display, item->note + 1, item->value,
                                 &item->browser_socket, &m->error);
        if (!item->argv)
            return true;
        m->chosen = item;
    } else if (item->action == ITEM_EMOJI || item->action == ITEM_CALCULATOR ||
               item->action == ITEM_SYSTEM || item->action == ITEM_CALENDAR || item->action == ITEM_FILES) {
        Mode mode = item->action == ITEM_EMOJI ? EMOJI :
                    item->action == ITEM_CALCULATOR ? CALCULATOR :
                    item->action == ITEM_SYSTEM ? SYSTEM :
                    item->action == ITEM_CALENDAR ? CALENDAR : FILES;
        if (enter_workflow(m, s, mode))
            usage_record(item);
        return true;
    } else {
        m->chosen = item;
    }
    m->done = m->accepted = true;
    return true;
}

bool model_dispatch(const Model *m, const Settings *s)
{
    if (!m->chosen)
        return true;
    g_autoptr(GError) error = NULL;
    GSpawnFlags flags = G_SPAWN_SEARCH_PATH;
    if (m->chosen->action == ITEM_WEB)
        flags |= G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL;
    bool success;
    if (m->chosen->browser_socket)
        success = web_send(m->chosen->browser_socket, m->chosen->argv, &error);
    else if (m->chosen->action == ITEM_APP)
        success = g_app_info_launch(m->chosen->app, NULL, NULL, &error);
    else
        success = g_spawn_async(NULL, m->chosen->argv, NULL, flags, NULL, NULL, NULL, &error);
    if (!success)
        fprintf(stderr, "launch: %s\n", error->message);
    else {
        if (m->chosen->action == ITEM_WEB)
            web_focus(s, m->chosen->note + 1);
        usage_record(m->chosen);
    }
    return success;
}
