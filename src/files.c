#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>
#include "launch.h"

typedef struct {
    FileMatch match;
    char *name_key, *path_key;
} Entry;

typedef struct {
    char *query;
    int number;
} Request;

typedef struct {
    const Entry *entry;
    int score;
} Ranked;

struct FileSearch {
    GAsyncQueue *requests, *results;
    char **roots, **exclude;
    char *cache;
    int fd, limit, cache_seconds;
    gint latest, closing;
    bool hidden;
};

static void entry_free(gpointer data)
{
    Entry *entry = data;
    g_free(entry->match.path);
    g_free(entry->name_key);
    g_free(entry->path_key);
    g_free(entry);
}

static void match_free(gpointer data)
{
    FileMatch *match = data;
    g_free(match->path);
    g_free(match);
}

static void request_free(gpointer data)
{
    Request *request = data;
    g_free(request->query);
    g_free(request);
}

static void result_free(gpointer data)
{
    FileResults *result = data;
    g_ptr_array_unref(result->matches);
    g_free(result);
}

static GPtrArray *new_entries(void)
{
    return g_ptr_array_new_with_free_func(entry_free);
}

static void add_entry(GPtrArray *entries, const char *path, bool directory)
{
    Entry *entry = g_new0(Entry, 1);
    entry->match = (FileMatch){g_strdup(path), directory};
    g_ptr_array_add(entries, entry);
}

static GPtrArray *read_cache(const FileSearch *search)
{
    GPtrArray *entries = new_entries();
    g_autofree char *data = NULL;
    gsize size;
    if (!g_file_get_contents(search->cache, &data, &size, NULL))
        return entries;
    for (const char *p = data; p < data + size && entries->len < (guint)search->limit;) {
        const char *end = memchr(p, '\0', data + size - p);
        if (!end || end - p < 2 || (*p != 'd' && *p != 'f') || !g_path_is_absolute(p + 1))
            break;
        add_entry(entries, p + 1, *p == 'd');
        p = end + 1;
    }
    return entries;
}

static void write_cache(const FileSearch *search, const GPtrArray *entries)
{
    g_autofree char *dir = g_path_get_dirname(search->cache);
    if (g_mkdir_with_parents(dir, 0700) != 0)
        return;
    g_autoptr(GString) data = g_string_new(NULL);
    for (guint i = 0; i < entries->len; i++) {
        const Entry *entry = g_ptr_array_index(entries, i);
        g_string_append_c(data, entry->match.directory ? 'd' : 'f');
        g_string_append_len(data, entry->match.path, strlen(entry->match.path) + 1);
    }
    g_file_set_contents_full(search->cache, data->str, data->len,
                             G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
}

static GPtrArray *scan(const FileSearch *search)
{
    GPtrArray *entries = new_entries();
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GQueue dirs = G_QUEUE_INIT;
    for (size_t i = 0; search->roots[i]; i++) {
        g_autofree char *root = realpath(search->roots[i], NULL);
        if (root && g_hash_table_add(seen, g_strdup(root)))
            g_queue_push_tail(&dirs, g_steal_pointer(&root));
    }
    while (!g_queue_is_empty(&dirs) && entries->len < (guint)search->limit &&
           !g_atomic_int_get(&search->closing)) {
        g_autofree char *dir = g_queue_pop_head(&dirs);
        g_autoptr(GDir) listing = g_dir_open(dir, 0, NULL);
        if (!listing)
            continue;
        const char *name;
        while ((name = g_dir_read_name(listing)) && entries->len < (guint)search->limit &&
               !g_atomic_int_get(&search->closing)) {
            if ((!search->hidden && name[0] == '.') ||
                g_strv_contains((const gchar *const *)search->exclude, name))
                continue;
            g_autofree char *path = g_build_filename(dir, name, NULL);
            struct stat info;
            if (lstat(path, &info) != 0 || (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)))
                continue;
            bool directory = S_ISDIR(info.st_mode);
            add_entry(entries, path, directory);
            if (directory && g_hash_table_add(seen, g_strdup(path)))
                g_queue_push_tail(&dirs, g_steal_pointer(&path));
        }
    }
    g_queue_clear_full(&dirs, g_free);
    return entries;
}

static int compare_rank(gconstpointer left, gconstpointer right)
{
    const Ranked *a = left, *b = right;
    if (a->score != b->score)
        return a->score > b->score ? -1 : 1;
    return strcmp(a->entry->match.path, b->entry->match.path);
}

static void publish(FileSearch *search, const Request *request, GPtrArray *entries, bool pending)
{
    g_auto(GStrv) tokens = g_strsplit_set(request->query, " \t\n\r", 0);
    g_autoptr(GArray) ranks = g_array_new(FALSE, FALSE, sizeof(Ranked));
    for (guint i = 0; i < entries->len; i++) {
        if (g_atomic_int_get(&search->latest) != request->number)
            return;
        Entry *entry = g_ptr_array_index(entries, i);
        if (!entry->name_key) {
            g_autofree char *name = g_filename_display_basename(entry->match.path);
            g_autofree char *display = g_filename_display_name(entry->match.path);
            entry->name_key = search_key(name);
            entry->path_key = search_key(display);
        }
        int score = search_score(entry->name_key, entry->path_key, request->query, tokens, false, true);
        if (strcmp(entry->name_key, request->query) == 0)
            score = 1000000;
        if (score >= 0) {
            Ranked rank = {entry, score};
            g_array_append_val(ranks, rank);
        }
    }
    g_array_sort(ranks, compare_rank);
    if (g_atomic_int_get(&search->latest) != request->number)
        return;

    FileResults *result = g_new(FileResults, 1);
    *result = (FileResults){request->number, g_ptr_array_new_with_free_func(match_free), pending};
    for (guint i = 0; i < MIN(ranks->len, 256u); i++) {
        const FileMatch *original = &g_array_index(ranks, Ranked, i).entry->match;
        FileMatch *match = g_new(FileMatch, 1);
        *match = (FileMatch){g_strdup(original->path), original->directory};
        g_ptr_array_add(result->matches, match);
    }
    g_async_queue_push(search->results, result);
    eventfd_write(search->fd, 1);
}

static void search_free(FileSearch *search)
{
    close(search->fd);
    g_async_queue_unref(search->requests);
    g_async_queue_unref(search->results);
    g_strfreev(search->roots);
    g_strfreev(search->exclude);
    g_free(search->cache);
    g_free(search);
}

static gpointer worker(gpointer data)
{
    FileSearch *search = data;
    g_autoptr(GPtrArray) entries = NULL;
    gint64 checked_at = 0;
    while (!g_atomic_int_get(&search->closing)) {
        Request *request = g_async_queue_pop(search->requests);
        Request *next;
        while ((next = g_async_queue_try_pop(search->requests))) {
            request_free(request);
            request = next;
        }
        if (!request->query || g_atomic_int_get(&search->closing)) {
            request_free(request);
            break;
        }
        gint64 now = g_get_monotonic_time();
        bool refresh = !checked_at || (search->cache_seconds &&
            now - checked_at >= search->cache_seconds * G_TIME_SPAN_SECOND);
        if (refresh) {
            gint64 age = 0;
            struct stat info;
            if (!entries && stat(search->cache, &info) == 0) {
                entries = read_cache(search);
                age = g_get_real_time() / G_TIME_SPAN_SECOND - info.st_mtime;
                refresh = age < 0 || age >= search->cache_seconds;
            }
            if (refresh) {
                if (entries && entries->len)
                    publish(search, request, entries, true);
                g_clear_pointer(&entries, g_ptr_array_unref);
                entries = scan(search);
                if (!g_atomic_int_get(&search->closing))
                    write_cache(search, entries);
            }
            checked_at = g_get_monotonic_time() - (refresh ? 0 : age * G_TIME_SPAN_SECOND);
        }
        publish(search, request, entries, false);
        request_free(request);
    }
    search_free(search);
    return NULL;
}

FileSearch *files_create(char **roots, char **exclude, bool hidden, int cache_seconds, int limit)
{
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0)
        return NULL;

    FileSearch *search = g_new0(FileSearch, 1);
    search->fd = fd;
    search->roots = g_strdupv(roots);
    search->exclude = g_strdupv(exclude);
    search->hidden = hidden;
    search->cache_seconds = cache_seconds;
    search->limit = limit;
    search->requests = g_async_queue_new_full(request_free);
    search->results = g_async_queue_new_full(result_free);

    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_autofree char *version = g_strdup_printf("1:%d:%d", hidden, limit);
    g_checksum_update(checksum, (const guchar *)version, strlen(version) + 1);
    char **lists[] = {roots, exclude};
    for (size_t i = 0; i < G_N_ELEMENTS(lists); i++) {
        for (size_t j = 0; lists[i][j]; j++)
            g_checksum_update(checksum, (const guchar *)lists[i][j], strlen(lists[i][j]) + 1);
        g_checksum_update(checksum, (const guchar *)"", 1);
    }
    g_autofree char *name = g_strconcat("files-", g_checksum_get_string(checksum), ".index", NULL);
    search->cache = g_build_filename(g_get_user_cache_dir(), "launch", name, NULL);
    g_autoptr(GError) error = NULL;
    GThread *thread = g_thread_try_new("launch-files", worker, search, &error);
    if (!thread) {
        search_free(search);
        return NULL;
    }
    g_thread_unref(thread);
    return search;
}

void files_close(FileSearch *search)
{
    if (!search)
        return;
    /* Hold a queue reference across shutdown, even if the worker exits immediately. */
    GAsyncQueue *requests = g_async_queue_ref(search->requests);
    g_atomic_int_inc(&search->latest);
    g_atomic_int_set(&search->closing, true);
    g_async_queue_push(requests, g_new0(Request, 1));
    g_async_queue_unref(requests);
}

int files_fd(const FileSearch *search)
{
    return search ? search->fd : -1;
}

int files_request(FileSearch *search, const char *query)
{
    int number = g_atomic_int_add(&search->latest, 1) + 1;
    if (query) {
        Request *request = g_new(Request, 1);
        *request = (Request){g_strdup(query), number};
        g_async_queue_push(search->requests, request);
    }
    return number;
}

FileResults *files_receive(FileSearch *search)
{
    eventfd_t wakeups;
    while (eventfd_read(search->fd, &wakeups) < 0 && errno == EINTR) {}
    return g_async_queue_try_pop(search->results);
}

void files_results_free(FileResults *result)
{
    result_free(result);
}
