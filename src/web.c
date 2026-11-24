#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include "web.h"

static const char placeholder[] = "{searchTerms}";

static GHashTable *load_bangs(const Settings *s)
{
    GHashTable *bangs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_autofree char *data = data_contents("config.ini");
    const char *start = data ? strstr(data, "\n# BEGIN HELIUM BANGS\n") : NULL;
    if (start) {
        const char *p = strchr(start + 1, '\n') + 1;
        const char *end = strstr(p, "# END HELIUM BANGS");
        while (end && p < end) {
            const char *next = strchr(p, '\n');
            const char *equals = next ? memchr(p, '=', next - p) : NULL;
            if (equals && equals > p + 2 && p[0] == '#' && p[1] == ' ')
                g_hash_table_insert(bangs, g_strndup(p + 2, equals - p - 2),
                                    g_strndup(equals + 1, next - equals - 1));
            if (!next)
                break;
            p = next + 1;
        }
    }
    g_auto(GStrv) keys = g_key_file_get_keys(s->file, "bangs", NULL, NULL);
    for (size_t i = 0; keys && keys[i]; i++) {
        const char *key = keys[i][0] == '!' ? keys[i] + 1 : keys[i];
        g_autofree char *trigger = g_utf8_strdown(key, -1);
        char *value = g_key_file_get_value(s->file, "bangs", keys[i], NULL);
        if (!value || !*g_strstrip(value)) {
            g_hash_table_remove(bangs, trigger);
            g_free(value);
        } else {
            g_hash_table_replace(bangs, g_steal_pointer(&trigger), value);
        }
    }
    return bangs;
}

static GUri *web_uri(const char *url)
{
    if (strpbrk(url, " \r\t\n"))
        return NULL;
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_ENCODED | G_URI_FLAGS_PARSE_RELAXED, NULL);
    if (!uri)
        return NULL;
    const char *host = g_uri_get_host(uri);
    const char *scheme = g_uri_get_scheme(uri);
    if (host && *host && scheme && (strcmp(scheme, "https") == 0 || strcmp(scheme, "http") == 0))
        return uri;
    g_uri_unref(uri);
    return NULL;
}

static char *build_url(const char *pattern, const char *terms)
{
    if (!strstr(pattern, placeholder))
        return g_strdup(pattern);

    /* Chromium search templates encode query spaces as plus signs. */
    const char *query = strchr(pattern, '?');
    const char *fragment = strchr(pattern, '#');
    const char *p = pattern, *insert;
    GString *url = g_string_new(NULL);
    while ((insert = strstr(p, placeholder))) {
        bool query_value = query && insert > query && (!fragment || insert < fragment);
        g_autofree char *escaped = g_uri_escape_string(terms, NULL, FALSE);
        g_string_append_len(url, p, insert - p);
        const char *chunk = escaped, *space;
        while (query_value && (space = strstr(chunk, "%20"))) {
            g_string_append_len(url, chunk, space - chunk);
            g_string_append_c(url, '+');
            chunk = space + 3;
        }
        g_string_append(url, chunk);
        p = insert + sizeof placeholder - 1;
    }
    g_string_append(url, p);
    return g_string_free(url, FALSE);
}

static size_t token_length(const char *text)
{
    return strcspn(text, " \t\r\n");
}

static char *last_token(char *text)
{
    size_t length = strlen(text);
    while (length && !g_ascii_isspace(text[length - 1]))
        length--;
    return text + length;
}

static bool profile_name(const char *text)
{
    return *text && strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
                    == strlen(text);
}

void web_target_clear(WebTarget *target)
{
    g_free(target->url);
    g_free(target->label);
    g_free(target->detail);
    g_free(target->profile);
    g_free(target->error);
}

bool web_target(GHashTable **bangs, const Settings *s, const char *query, WebTarget *target)
{
    g_autofree char *input = g_strdup(query);
    char *text = g_strstrip(input);
    char *last = last_token(text);
    g_autofree char *profile = NULL;
    if (last > text && *last == '@') {
        profile = g_strdup(last + 1);
        *last = '\0';
        g_strchomp(text);
        last = last_token(text);
    }

    bool direct_url = g_ascii_strncasecmp(text, "https://", 8) == 0 ||
                      g_ascii_strncasecmp(text, "http://", 7) == 0;
    if (direct_url) {
        g_autoptr(GUri) uri = web_uri(text);
        if (!uri) {
            target->error = g_strdup("Invalid web address");
            return true;
        }
        target->url = g_strdup(text);
        target->label = g_strdup(g_uri_get_host(uri));
        target->detail = g_strdup(text);
    } else {
        char *bang = *text == '!' ? text : *last == '!' ? last : NULL;
        if (!bang)
            return false;
        size_t length = token_length(bang);
        g_autofree char *trigger = g_strndup(bang + 1, length - 1);
        char *suffix = *trigger ? strchr(trigger + 1, '@') : NULL;
        if (suffix) {
            if (profile) {
                target->error = g_strdup("Choose one browser profile");
                return true;
            }
            profile = g_strdup(suffix + 1);
            *suffix = '\0';
        }
        if (!*trigger) {
            target->error = g_strdup("Enter a bang such as !g, !gai, or !w");
            return true;
        }
        if (!*bangs)
            *bangs = load_bangs(s);
        g_autofree char *key = g_utf8_strdown(trigger, -1);
        const char *definition = g_hash_table_lookup(*bangs, key);
        if (!definition) {
            target->error = g_strdup_printf("Unknown bang !%s", trigger);
            return true;
        }
        const char *label = strchr(definition, '\t');
        g_autofree char *pattern = label ? g_strndup(definition, label - definition) : g_strdup(definition);
        g_strchomp(pattern);
        char *terms;
        if (bang == text) {
            terms = g_strstrip(bang + length);
        } else {
            *bang = '\0';
            terms = g_strchomp(text);
        }
        g_autofree char *url = build_url(pattern, terms);
        g_autoptr(GUri) uri = web_uri(url);
        if (!uri) {
            target->error = g_strdup_printf("Invalid URL template for !%s", trigger);
            return true;
        }
        if (!*terms && strstr(pattern, placeholder)) {
            g_free(url);
            url = g_uri_join(G_URI_FLAGS_ENCODED, g_uri_get_scheme(uri), NULL, g_uri_get_host(uri),
                             g_uri_get_port(uri), "/", NULL, NULL);
        }
        target->label = g_strdup(label && label[1] ? label + 1 : g_uri_get_host(uri));
        target->detail = *terms ? g_strdup_printf("!%s · %s", trigger, terms)
                                : g_strdup_printf("!%s · %s", trigger, g_uri_get_host(uri));
        target->url = g_steal_pointer(&url);
    }

    if (!profile)
        profile = g_key_file_get_string(s->file, "browser", "default-profile", NULL);
    if (!profile)
        profile = g_strdup("personal");
    if (!profile_name(profile)) {
        target->error = g_strdup("Enter a browser profile such as @work or @personal");
        return true;
    }
    target->profile = g_steal_pointer(&profile);
    g_autofree char *group = g_strconcat("browser ", target->profile, NULL);
    if (!g_key_file_has_group(s->file, group))
        target->error = g_strdup_printf("Browser profile @%s is not configured", target->profile);
    return true;
}

static unsigned char *x_property(Display *display, Window window, Atom property, Atom type,
                                 unsigned long limit, unsigned long *count)
{
    Atom found;
    int format;
    unsigned long remaining;
    unsigned char *data = NULL;
    if (XGetWindowProperty(display, window, property, 0, limit, False, type,
                           &found, &format, count, &remaining, &data) == Success &&
        found == type && format == 32 && !remaining)
        return data;
    if (data)
        XFree(data);
    *count = 0;
    return NULL;
}

static bool browser_window(Display *display, Window window, const char *class_name,
                           const char *directory_suffix, guint64 lock_pid, pid_t *pid)
{
    XClassHint hint;
    if (!XGetClassHint(display, window, &hint))
        return false;
    bool matches = g_strcmp0(hint.res_class, class_name) == 0;
    bool instance_matches = directory_suffix && hint.res_name &&
                            g_str_has_suffix(hint.res_name, directory_suffix);
    XFree(hint.res_name);
    XFree(hint.res_class);
    if (!matches)
        return false;

    Window owner;
    if (XGetTransientForHint(display, window, &owner))
        return false;
    unsigned long count;
    Atom kind = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    Atom normal = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    Atom *types = (Atom *)x_property(display, window, kind, XA_ATOM, 32, &count);
    matches = !count || types[0] == normal;
    if (types)
        XFree(types);
    if (!matches)
        return false;

    pid_t candidate = 0;
    if (directory_suffix) {
        Atom pid_atom = XInternAtom(display, "_NET_WM_PID", False);
        unsigned long *owner_pid = (unsigned long *)x_property(display, window, pid_atom,
                                                              XA_CARDINAL, 1, &count);
        if (count == 1 && *owner_pid > 0 && *owner_pid <= G_MAXINT &&
            (*owner_pid == lock_pid || instance_matches))
            candidate = (pid_t)*owner_pid;
        if (owner_pid)
            XFree(owner_pid);
        if (!candidate)
            return false;

        Atom role = XInternAtom(display, "WM_WINDOW_ROLE", False);
        XTextProperty text = {0};
        matches = XGetTextProperty(display, window, &text, role) &&
                  text.format == 8 && text.nitems == 7 && memcmp(text.value, "browser", 7) == 0;
        if (text.value)
            XFree(text.value);
        if (!matches)
            return false;
    }

    XWindowAttributes attributes;
    if (!XGetWindowAttributes(display, window, &attributes) || attributes.override_redirect)
        return false;
    matches = attributes.map_state == IsViewable;
    if (!matches) {
        Atom state = XInternAtom(display, "WM_STATE", False);
        unsigned long *wm_state = (unsigned long *)x_property(display, window, state, state, 2, &count);
        matches = count == 2 && (*wm_state == NormalState || *wm_state == IconicState);
        if (wm_state)
            XFree(wm_state);
    }
    if (matches)
        *pid = candidate;
    return matches;
}

static bool browser_is_open(Display *display, const char *class_name, const char *directory,
                            guint64 lock_pid, pid_t *pid)
{
    g_autofree char *suffix = directory ? g_strdup_printf(" (%s)", directory) : NULL;
    Window root = DefaultRootWindow(display), ignored;
    unsigned long count;
    Atom client_list = XInternAtom(display, "_NET_CLIENT_LIST", False);
    Window *windows = (Window *)x_property(display, root, client_list, XA_WINDOW, 65536, &count);
    if (!windows) {
        unsigned int children;
        if (!XQueryTree(display, root, &ignored, &ignored, &windows, &children))
            return false;
        count = children;
    }
    bool found = false;
    for (unsigned long i = 0; i < count && !found; i++)
        found = browser_window(display, windows[i], class_name, suffix, lock_pid, pid);
    if (windows)
        XFree(windows);
    return found;
}

/* A Chromium user-data directory names the running singleton on this host. */
static guint64 profile_lock_pid(const char *directory)
{
    g_autofree char *lock = g_build_filename(directory, "SingletonLock", NULL);
    g_autofree char *owner = g_file_read_link(lock, NULL);
    if (!owner)
        return 0;
    const char *separator = strrchr(owner, '-');
    if (!separator || strlen(g_get_host_name()) != (size_t)(separator - owner) ||
        strncmp(owner, g_get_host_name(), separator - owner) != 0)
        return 0;
    guint64 pid = 0;
    return g_ascii_string_to_unsigned(separator + 1, 10, 1, G_MAXINT, &pid, NULL) ? pid : 0;
}

static GSocket *connect_browser(const char *path, pid_t pid)
{
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISSOCK(info.st_mode) || info.st_uid != getuid())
        return NULL;
    g_autoptr(GSocket) socket = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                                           G_SOCKET_PROTOCOL_DEFAULT, NULL);
    if (!socket)
        return NULL;
    g_socket_set_timeout(socket, 2);
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(path);
    if (!g_socket_connect(socket, address, NULL, NULL))
        return NULL;
    g_autoptr(GCredentials) peer = g_socket_get_credentials(socket, NULL);
    if (!peer || g_credentials_get_unix_pid(peer, NULL) != pid ||
        g_credentials_get_unix_user(peer, NULL) != getuid())
        return NULL;
    return g_steal_pointer(&socket);
}

/* The profile link can be removed while Chromium's temporary socket remains open. */
static GSocket *find_browser_socket(const char *directory, pid_t pid)
{
    g_autofree char *path = g_build_filename(directory, "SingletonSocket", NULL);
    GSocket *socket = connect_browser(path, pid);
    if (socket)
        return socket;

    g_autofree char *proc = g_strdup_printf("/proc/%ld", (long)pid);
    g_autofree char *fds = g_build_filename(proc, "fd", NULL);
    g_autoptr(GDir) listing = g_dir_open(fds, 0, NULL);
    if (!listing)
        return NULL;
    g_autoptr(GHashTable) inodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    const char *name;
    while ((name = g_dir_read_name(listing))) {
        g_autofree char *fd = g_build_filename(fds, name, NULL);
        g_autofree char *target = g_file_read_link(fd, NULL);
        if (target && g_str_has_prefix(target, "socket:["))
            g_hash_table_add(inodes, g_steal_pointer(&target));
    }
    g_autofree char *table = g_build_filename(proc, "net", "unix", NULL);
    g_autofree char *contents = NULL;
    if (!g_file_get_contents(table, &contents, NULL, NULL))
        return NULL;
    g_auto(GStrv) lines = g_strsplit(contents, "\n", 0);
    const unsigned listening = 0x10000; /* Linux __SO_ACCEPTCON in /proc/net/unix */
    for (size_t i = 1; lines[i]; i++) {
        unsigned flags, type;
        unsigned long inode;
        int offset = 0;
        if (sscanf(lines[i], "%*s %*s %*s %x %x %*s %lu %n", &flags, &type, &inode, &offset) != 3 ||
            !offset || !(flags & listening) || type != SOCK_STREAM)
            continue;
        const char *candidate = lines[i] + offset;
        if (!g_path_is_absolute(candidate) || !g_str_has_suffix(candidate, "/SingletonSocket"))
            continue;
        g_autofree char *key = g_strdup_printf("socket:[%lu]", inode);
        if (g_hash_table_contains(inodes, key) && (socket = connect_browser(candidate, pid)))
            return socket;
    }
    return NULL;
}

bool web_send(GSocket *socket, char **argv, GError **error)
{
    /* Chromium ProcessSingleton accepts START, cwd and argv separated by NUL bytes. */
    g_autofree char *cwd = g_get_current_dir();
    g_autoptr(GString) request = g_string_new("START");
    g_string_append_c(request, '\0');
    g_string_append(request, cwd);
    for (size_t i = 0; argv[i]; i++) {
        g_string_append_c(request, '\0');
        g_string_append(request, argv[i]);
    }
    for (size_t sent = 0; sent < request->len;) {
        gssize length = g_socket_send(socket, request->str + sent, request->len - sent, NULL, error);
        if (length < 0)
            return false;
        if (!length)
            break;
        sent += (size_t)length;
    }
    if (!g_socket_shutdown(socket, false, true, error))
        return false;
    char reply[3];
    size_t used = 0;
    while (used < sizeof reply) {
        gssize length = g_socket_receive(socket, reply + used, sizeof reply - used, NULL, error);
        if (length < 0)
            return false;
        if (!length)
            break;
        used += (size_t)length;
    }
    if (used == sizeof reply && memcmp(reply, "ACK", sizeof reply) == 0)
        return true;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The browser did not accept the tab");
    return false;
}

void web_focus(const Settings *s, const char *profile)
{
    g_autofree char *group = g_strconcat("browser ", profile, NULL);
    g_autofree char *command = g_key_file_get_string(s->file, group, "focus-command", NULL);
    if (!command || !*command)
        return;
    g_auto(GStrv) argv = NULL;
    g_autoptr(GError) error = NULL;
    if (!g_shell_parse_argv(command, NULL, &argv, &error) ||
        !g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                       NULL, NULL, NULL, &error))
        fprintf(stderr, "launch: [%s] focus-command: %s\n", group, error->message);
}

char **web_command(const Settings *s, Display *display, const char *profile, const char *url,
                   GSocket **connection, char **error)
{
    g_autofree char *group = g_strconcat("browser ", profile, NULL);
    g_autofree char *command = g_key_file_get_string(s->file, group, "command", NULL);
    g_autofree char *class_name = g_key_file_get_string(s->file, group, "window-class", NULL);
    g_auto(GStrv) argv = NULL;
    int argc;
    if (!command || !class_name || !*class_name || !g_shell_parse_argv(command, &argc, &argv, NULL)) {
        *error = g_strdup_printf("Set command and window-class in [%s]", group);
        return NULL;
    }
    const char *directory = NULL, *profile_arg = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--new-window") == 0 || strcmp(argv[i], "--same-tab") == 0) {
            *error = g_strdup_printf("Use an existing-window tab command for @%s", profile);
            return NULL;
        }
        const char *equals = strchr(argv[i], '=');
        const char *path = equals ? equals + 1 : argv[i];
        if (*path == '~') {
            g_autofree char *expanded = expand_home(path);
            g_autofree char *prefix = g_strndup(argv[i], path - argv[i]);
            g_free(argv[i]);
            argv[i] = g_strconcat(prefix, expanded, NULL);
        }
        if (g_str_has_prefix(argv[i], "--user-data-dir="))
            directory = argv[i] + strlen("--user-data-dir=");
        else if (g_str_has_prefix(argv[i], "--profile-directory="))
            profile_arg = argv[i];
    }
    if (directory && !g_path_is_absolute(directory)) {
        *error = g_strdup_printf("Use an absolute browser data directory for @%s", profile);
        return NULL;
    }
    g_autofree char *canonical = directory ? g_canonicalize_filename(directory, NULL) : NULL;
    guint64 lock_pid = directory ? profile_lock_pid(directory) : 0;
    pid_t pid = 0;
    if (!browser_is_open(display, class_name, canonical, lock_pid, &pid)) {
        *error = g_strdup_printf("Open a browser window in @%s first", profile);
        return NULL;
    }
    if (directory) {
        *connection = find_browser_socket(directory, pid);
        if (!*connection) {
            *error = g_strdup_printf("Could not connect to the browser window in @%s", profile);
            return NULL;
        }
        char **args = g_new0(char *, 4);
        args[0] = g_strdup("chromium");
        args[1] = g_strdup(profile_arg ? profile_arg : url);
        if (profile_arg)
            args[2] = g_strdup(url);
        return args;
    }
    g_autofree char *exe = g_find_program_in_path(argv[0]);
    if (!exe) {
        *error = g_strdup_printf("Browser command not found for @%s", profile);
        return NULL;
    }
    char **args = g_renew(char *, g_steal_pointer(&argv), argc + 2);
    args[argc] = g_strdup(url);
    args[argc + 1] = NULL;
    return args;
}
