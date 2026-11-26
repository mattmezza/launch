#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "rates.h"

/* Known ECB codes also allow recognition before the first download. */
static const char *const codes[] = {
    "AUD", "BRL", "CAD", "CHF", "CNY", "CZK", "DKK", "EUR", "GBP", "HKD", "HUF",
    "IDR", "ILS", "INR", "ISK", "JPY", "KRW", "MXN", "MYR", "NOK", "NZD", "PHP",
    "PLN", "RON", "SEK", "SGD", "THB", "TRY", "USD", "ZAR",
};

enum { MAX_FEED_SIZE = 65536, REFRESH_SECONDS = 6 * 3600, RETRY_SECONDS = 5 * 60 };

typedef struct {
    double values[G_N_ELEMENTS(codes)];
    char date[11];
} Feed;

struct Rates {
    Feed feed;
    int fd;
};

int rates_currency(const char *code)
{
    for (size_t i = 0; i < G_N_ELEMENTS(codes); i++)
        if (g_ascii_strcasecmp(code, codes[i]) == 0)
            return (int)i;
    return -1;
}

const char *rates_code(int currency)
{
    return currency >= 0 && (size_t)currency < G_N_ELEMENTS(codes) ? codes[currency] : NULL;
}

double rates_value(const Rates *rates, int currency)
{
    return rates ? rates->feed.values[currency] : 0;
}

const char *rates_date(const Rates *rates)
{
    return rates && rates->feed.date[0] ? rates->feed.date : NULL;
}

static const char *attribute(const char **names, const char **values, const char *name)
{
    for (size_t i = 0; names[i]; i++)
        if (strcmp(names[i], name) == 0)
            return values[i];
    return NULL;
}

static bool valid_date(const char *date)
{
    if (strlen(date) != 10)
        return false;
    for (size_t i = 0; i < 10; i++)
        if (i == 4 || i == 7 ? date[i] != '-' : !g_ascii_isdigit(date[i]))
            return false;
    return g_date_valid_dmy((GDateDay)atoi(date + 8), (GDateMonth)atoi(date + 5),
                            (GDateYear)atoi(date));
}

static void parse_cube(GMarkupParseContext *xml, const char *name, const char **names,
                       const char **values, gpointer data, GError **error)
{
    if (strcmp(name, "Cube") != 0)
        return;
    Feed *feed = data;
    const char *date = attribute(names, values, "time");
    const char *code = attribute(names, values, "currency");
    if (date) {
        if (feed->date[0] || !valid_date(date)) {
            g_set_error_literal(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                                "Invalid ECB reference date");
            return;
        }
        memcpy(feed->date, date, sizeof feed->date);
    } else if (code) {
        int currency = rates_currency(code);
        if (currency < 0)
            return;
        const GSList *stack = g_markup_parse_context_get_element_stack(xml);
        const char *rate = attribute(names, values, "rate");
        char *end;
        double value = rate ? g_ascii_strtod(rate, &end) : 0;
        if (!feed->date[0] || !stack->next || strcmp(stack->next->data, "Cube") != 0 ||
            !rate || *end || !isfinite(value) || value <= 0 || feed->values[currency]) {
            g_set_error_literal(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                                "Invalid ECB reference rate");
            return;
        }
        feed->values[currency] = value;
    }
}

static bool parse_feed(const char *text, gsize length, Feed *feed)
{
    if (!length || length > MAX_FEED_SIZE)
        return false;
    static const GMarkupParser parser = {.start_element = parse_cube};
    Feed parsed = {0};
    int euro = rates_currency("EUR");
    parsed.values[euro] = 1;
    g_autoptr(GMarkupParseContext) xml = g_markup_parse_context_new(&parser, 0, &parsed, NULL);
    if (!g_markup_parse_context_parse(xml, text, (gssize)length, NULL) ||
        !g_markup_parse_context_end_parse(xml, NULL) || !parsed.date[0])
        return false;
    /* A dated but empty response must not replace an existing cache. */
    for (size_t i = 0; i < G_N_ELEMENTS(codes); i++) {
        if ((int)i != euro && parsed.values[i]) {
            *feed = parsed;
            return true;
        }
    }
    return false;
}

static char *cache_path(void)
{
    return g_build_filename(g_get_user_cache_dir(), "launch", "ecb-rates.xml", NULL);
}

static bool fresh(struct stat info, int seconds)
{
    time_t now = time(NULL);
    return info.st_mtime <= now && difftime(now, info.st_mtime) < seconds;
}

static bool load_cache(const char *path, Feed *feed)
{
    struct stat info;
    g_autofree char *text = NULL;
    gsize length;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_size <= MAX_FEED_SIZE &&
           g_file_get_contents(path, &text, &length, NULL) && parse_feed(text, length, feed);
}

Rates *rates_open(void)
{
    Rates *rates = g_new0(Rates, 1);
    rates->fd = -1;
    g_autofree char *path = cache_path();
    struct stat info;
    if (load_cache(path, &rates->feed) && stat(path, &info) == 0 && fresh(info, REFRESH_SECONDS))
        return rates;

    /* The helper can finish saving the cache even if Launch has already closed. */
    char *argv[] = {"/proc/self/exe", "launch", "--refresh-rates", NULL};
    GSpawnFlags flags = G_SPAWN_FILE_AND_ARGV_ZERO | G_SPAWN_STDERR_TO_DEV_NULL |
                        G_SPAWN_CLOEXEC_PIPES;
    if (g_spawn_async_with_pipes(NULL, argv, NULL, flags, NULL, NULL, NULL,
                                 NULL, &rates->fd, NULL, NULL)) {
        if (!g_unix_set_fd_nonblocking(rates->fd, TRUE, NULL)) {
            close(rates->fd);
            rates->fd = -1;
        }
    }
    return rates;
}

void rates_close(Rates *rates)
{
    if (!rates)
        return;
    if (rates->fd >= 0)
        close(rates->fd);
    g_free(rates);
}

int rates_fd(const Rates *rates)
{
    return rates ? rates->fd : -1;
}

bool rates_receive(Rates *rates)
{
    if (!rates || rates->fd < 0)
        return false;
    char byte;
    if (read(rates->fd, &byte, 1) < 0 && (errno == EINTR || errno == EAGAIN))
        return false;
    close(rates->fd);
    rates->fd = -1;
    g_autofree char *path = cache_path();
    load_cache(path, &rates->feed);
    return true;
}

static bool download(const char *path, const Feed *cached)
{
    char *argv[] = {"curl", "-q", "--fail", "--silent", "--proto", "=https",
                    "--connect-timeout", "3", "--max-time", "8", "--max-filesize", "65536",
                    "https://www.ecb.europa.eu/stats/eurofxref/eurofxref-daily.xml", NULL};
    g_autofree char *text = NULL;
    int status;
    Feed feed;
    return g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                        NULL, NULL, &text, NULL, &status, NULL) &&
           g_spawn_check_wait_status(status, NULL) && parse_feed(text, strlen(text), &feed) &&
           strcmp(feed.date, cached->date) >= 0 &&
           g_file_set_contents_full(path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
}

bool rates_refresh(void)
{
    g_autofree char *path = cache_path();
    g_autofree char *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) < 0)
        return false;
    g_autofree char *lock_path = g_strconcat(path, ".lock", NULL);
    int lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0)
        return false;
    bool success = false;
    if (flock(lock, LOCK_EX) == 0) {
        Feed cached = {0};
        struct stat info;
        if (load_cache(path, &cached) && stat(path, &info) == 0 && fresh(info, REFRESH_SECONDS)) {
            success = true;
        } else if (fstat(lock, &info) == 0 && (!info.st_size || !fresh(info, RETRY_SECONDS))) {
            /* Keep the last attempt time so several Launch instances share one retry window. */
            if (ftruncate(lock, 1) == 0 && futimens(lock, NULL) == 0)
                success = download(path, &cached);
        }
    }
    close(lock);
    return success;
}
