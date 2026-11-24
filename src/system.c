#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "system.h"

static Counters cpu_counters(void)
{
    FILE *file = fopen("/proc/stat", "r");
    if (!file)
        return (Counters){0};
    uint64_t ticks[8] = {0};
    int read = fscanf(file, "cpu %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                      " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
                      &ticks[0], &ticks[1], &ticks[2], &ticks[3],
                      &ticks[4], &ticks[5], &ticks[6], &ticks[7]);
    fclose(file);
    uint64_t total = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(ticks); i++)
        total += ticks[i];
    return (Counters){.a = total, .b = ticks[3] + ticks[4], .valid = read >= 4};
}

static double memory(System *s)
{
    uint64_t total = 0, available = 0;
    bool found = false;
    FILE *file = fopen("/proc/meminfo", "r");
    if (file) {
        char line[160];
        while (fgets(line, sizeof line, file)) {
            if (sscanf(line, "MemTotal: %" SCNu64, &total) == 1)
                continue;
            if (sscanf(line, "MemAvailable: %" SCNu64, &available) == 1) {
                found = true;
                break;
            }
        }
        fclose(file);
    }
    s->memory_total = found ? total * 1024 : 0;
    s->memory_used = found ? (total - MIN(available, total)) * 1024 : 0;
    return s->memory_total ? 100.0 * s->memory_used / s->memory_total : NAN;
}

static void add_counters(Counters *c, const char *device, uint64_t a, uint64_t b)
{
    c->a += a;
    c->b += b;
    c->devices++;
    c->identity += g_str_hash(device);
    c->valid = true;
}

static Counters disk_counters(void)
{
    FILE *file = fopen("/proc/diskstats", "r");
    Counters result = {0};
    if (!file)
        return result;
    char line[512], name[64], path[128];
    while (fgets(line, sizeof line, file)) {
        uint64_t reads, writes;
        if (sscanf(line, " %*s %*s %63s %*s %*s %" SCNu64 " %*s %*s %*s %" SCNu64,
                   name, &reads, &writes) != 3)
            continue;
        g_snprintf(path, sizeof path, "/sys/block/%s/device", name);
        if (g_file_test(path, G_FILE_TEST_IS_SYMLINK))
            add_counters(&result, name, reads, writes);
    }
    fclose(file);
    return result;
}

static Counters network_counters(void)
{
    FILE *file = fopen("/proc/net/dev", "r");
    Counters result = {0};
    if (!file)
        return result;
    char line[512];
    while (fgets(line, sizeof line, file)) {
        char *separator = strchr(line, ':');
        if (!separator)
            continue;
        *separator = '\0';
        char *name = g_strstrip(line);
        uint64_t rx, tx;
        if (strcmp(name, "lo") != 0 &&
            sscanf(separator + 1, " %" SCNu64 " %*s %*s %*s %*s %*s %*s %*s %" SCNu64, &rx, &tx) == 2)
            add_counters(&result, name, rx, tx);
    }
    fclose(file);
    return result;
}

static bool delta(Counters current, Counters *previous, double *a, double *b)
{
    bool valid = current.valid && previous->valid && current.devices == previous->devices &&
                 current.identity == previous->identity && current.a >= previous->a && current.b >= previous->b;
    if (valid) {
        *a = (double)(current.a - previous->a);
        *b = (double)(current.b - previous->b);
    }
    *previous = current;
    return valid;
}

static char *first_matching(const char *pattern)
{
    glob_t matches = {0};
    char *result = NULL;
    if (glob(pattern, 0, NULL, &matches) == 0)
        for (size_t i = 0; i < matches.gl_pathc; i++)
            if (access(matches.gl_pathv[i], R_OK) == 0) {
                result = g_strdup(matches.gl_pathv[i]);
                break;
            }
    globfree(&matches);
    return result;
}

static int intel_busy(void)
{
    const char *pmu = "/sys/bus/event_source/devices/i915";
    g_autofree char *type = NULL, *busy = NULL, *mask = NULL;
    g_autofree char *type_path = g_build_filename(pmu, "type", NULL);
    g_autofree char *event_path = g_build_filename(pmu, "events", "rcs0-busy", NULL);
    g_autofree char *mask_path = g_build_filename(pmu, "cpumask", NULL);
    if (!g_file_get_contents(type_path, &type, NULL, NULL) ||
        !g_file_get_contents(event_path, &busy, NULL, NULL) || !g_str_has_prefix(busy, "config="))
        return -1;
    struct perf_event_attr attr = {
        .size = sizeof attr,
        .type = (uint32_t)g_ascii_strtoull(type, NULL, 10),
        .config = g_ascii_strtoull(busy + strlen("config="), NULL, 0),
        .read_format = PERF_FORMAT_TOTAL_TIME_RUNNING,
    };
    int cpu = g_file_get_contents(mask_path, &mask, NULL, NULL) ? atoi(mask) : 0;
    return (int)syscall(SYS_perf_event_open, &attr, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC);
}

void system_open(System *s, int interval_ms)
{
    *s = (System){.gpu_fd = -1, .interval_ms = interval_ms};
    s->gpu_path = first_matching("/sys/class/drm/card[0-9]*/device/gpu_busy_percent");
    if (s->gpu_path)
        return;
    s->gpu_fd = intel_busy();
    if (s->gpu_fd < 0) {
        s->gpu_path = first_matching("/sys/class/drm/card[0-9]*/gt_act_freq_mhz");
        if (!s->gpu_path)
            s->gpu_path = first_matching("/sys/class/drm/card[0-9]*/device/tile0/gt0/freq0/act_freq");
        s->gpu_frequency = s->gpu_path != NULL;
    }
}

void system_step_interval(System *s, bool faster)
{
    const int intervals[] = {SYSTEM_MIN_INTERVAL_MS, 500, 1000, 2000, 5000, SYSTEM_MAX_INTERVAL_MS};
    int interval = s->interval_ms;
    for (size_t i = 0; i < G_N_ELEMENTS(intervals); i++) {
        if (faster && intervals[i] < s->interval_ms)
            interval = intervals[i];
        if (!faster && intervals[i] > s->interval_ms) {
            interval = intervals[i];
            break;
        }
    }
    s->interval_ms = interval;
}

void system_close(System *s)
{
    if (s->gpu_fd >= 0)
        close(s->gpu_fd);
    g_clear_pointer(&s->gpu_path, g_free);
    s->gpu_fd = -1;
}

static double gpu(System *s)
{
    if (s->gpu_fd >= 0) {
        uint64_t info[2];
        Counters current = {0};
        if (read(s->gpu_fd, info, sizeof info) == (ssize_t)sizeof info)
            current = (Counters){.a = info[0], .b = info[1], .valid = true};
        double busy, elapsed;
        return delta(current, &s->intel, &busy, &elapsed) && elapsed > 0
            ? CLAMP(100 * busy / elapsed, 0, 100) : NAN;
    }
    if (s->gpu_path) {
        FILE *file = fopen(s->gpu_path, "r");
        if (file) {
            double value;
            bool valid = fscanf(file, "%lf", &value) == 1;
            fclose(file);
            if (valid && value >= 0)
                return s->gpu_frequency ? value : MIN(100, value);
        }
    }
    return NAN;
}

static size_t sample_index(const System *s, size_t position)
{
    return (s->next + SYSTEM_HISTORY - s->count + position) % SYSTEM_HISTORY;
}

void system_sample(System *s, gint64 now)
{
    double seconds = (double)(now - s->last_sample) / G_USEC_PER_SEC;
    s->last_sample = now;
    s->timestamps[s->next] = now;
    double *point = s->points[s->next];
    for (size_t i = 0; i < SYSTEM_SERIES; i++)
        point[i] = NAN;
    double total, idle, a, b;
    if (delta(cpu_counters(), &s->cpu, &total, &idle) && total > 0)
        point[CPU_LOAD] = CLAMP(100.0 * (total - idle) / total, 0, 100);
    point[GPU_LOAD] = gpu(s);
    point[RAM_LOAD] = memory(s);
    if (delta(disk_counters(), &s->disk, &a, &b) && seconds > 0) {
        /* Linux diskstats reports sectors in 512-byte units. */
        point[DISK_READ] = a * 512 / seconds;
        point[DISK_WRITE] = b * 512 / seconds;
    }
    if (delta(network_counters(), &s->network, &a, &b) && seconds > 0) {
        point[NET_IN] = a / seconds;
        point[NET_OUT] = b / seconds;
    }
    struct statvfs disk;
    if (statvfs("/", &disk) == 0) {
        s->disk_total = (uint64_t)disk.f_frsize * disk.f_blocks;
        s->disk_used = s->disk_total - (uint64_t)disk.f_frsize * disk.f_bfree;
    }
    s->next = (s->next + 1) % SYSTEM_HISTORY;
    s->count = MIN(s->count + 1, (size_t)SYSTEM_HISTORY);
    gint64 oldest = now - SYSTEM_HISTORY_SECONDS * G_TIME_SPAN_SECOND;
    while (s->count && s->timestamps[sample_index(s, 0)] < oldest)
        s->count--;
}

double system_point(const System *s, Series series, size_t position)
{
    return position < s->count ? s->points[sample_index(s, position)][series] : NAN;
}

double system_position(const System *s, size_t position)
{
    double age = (double)(s->last_sample - s->timestamps[sample_index(s, position)]);
    return CLAMP(1 - age / (SYSTEM_HISTORY_SECONDS * (double)G_TIME_SPAN_SECOND), 0, 1);
}
