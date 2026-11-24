#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include "launch.h"

static char *usage_directory(void)
{
    const char *state = g_getenv("XDG_STATE_HOME");
    g_autofree char *fallback = NULL;
    if (!state || !g_path_is_absolute(state)) {
        fallback = g_build_filename(g_get_home_dir(), ".local", "state", NULL);
        state = fallback;
    }
    return g_build_filename(state, "launch", NULL);
}

static char *usage_key(const Item *item)
{
    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, item->usage_id, -1);
}

static int count(GKeyFile *file, const char *key)
{
    gint64 launches = g_key_file_get_int64(file, key, "count", NULL);
    return CLAMP(launches, 0, G_MAXINT - 1);
}

void usage_load(GPtrArray *items)
{
    g_autofree char *dir = usage_directory();
    g_autofree char *path = g_build_filename(dir, "usage.ini", NULL);
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL))
        return;
    for (guint i = 0; i < items->len; i++) {
        Item *item = g_ptr_array_index(items, i);
        if (!item->usage_id)
            continue;
        g_autofree char *key = usage_key(item);
        item->launches = count(file, key);
        item->last_used = g_key_file_get_int64(file, key, "last-used", NULL);
    }
}

void usage_record(const Item *item)
{
    if (!item->usage_id)
        return;
    g_autofree char *dir = usage_directory();
    if (g_mkdir_with_parents(dir, 0700) != 0)
        return;
    g_autofree char *lock = g_build_filename(dir, "usage.lock", NULL);
    int fd = open(lock, O_CREAT | O_CLOEXEC | O_RDWR, 0600);
    if (fd < 0)
        return;
    int status;
    do {
        status = flock(fd, LOCK_EX);
    } while (status < 0 && errno == EINTR);
    if (status == 0) {
        g_autofree char *path = g_build_filename(dir, "usage.ini", NULL);
        g_autofree char *key = usage_key(item);
        g_autoptr(GKeyFile) file = g_key_file_new();
        g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL);
        g_key_file_set_int64(file, key, "count", count(file, key) + 1);
        g_key_file_set_int64(file, key, "last-used", g_get_real_time());
        gsize size;
        g_autofree char *data = g_key_file_to_data(file, &size, NULL);
        g_file_set_contents_full(path, data, (gssize)size, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
    }
    close(fd);
}
