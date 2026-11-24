#ifndef LAUNCH_SYSTEM_H
#define LAUNCH_SYSTEM_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum { CPU, GPU, RAM, DISK, NETWORK, SYSTEM_TABS } SystemTab;
typedef enum { CPU_LOAD, GPU_LOAD, RAM_LOAD, DISK_READ, DISK_WRITE, NET_IN, NET_OUT, SYSTEM_SERIES } Series;
enum {
    SYSTEM_MIN_INTERVAL_MS = 250, SYSTEM_MAX_INTERVAL_MS = 10000,
    SYSTEM_HISTORY_SECONDS = 60,
    SYSTEM_HISTORY = SYSTEM_HISTORY_SECONDS * 1000 / SYSTEM_MIN_INTERVAL_MS + 1
};

typedef struct {
    uint64_t a, b;
    guint devices, identity;
    bool valid;
} Counters;

typedef struct {
    double points[SYSTEM_HISTORY][SYSTEM_SERIES];
    gint64 timestamps[SYSTEM_HISTORY];
    size_t next, count;
    gint64 last_sample;
    int interval_ms;
    Counters cpu, disk, network, intel;
    uint64_t memory_total, memory_used, disk_total, disk_used;
    char *gpu_path;
    int gpu_fd;
    bool gpu_frequency;
} System;

void system_open(System *, int interval_ms);
void system_close(System *);
void system_sample(System *, gint64 now);
void system_step_interval(System *, bool faster);
double system_point(const System *, Series, size_t position);
double system_position(const System *, size_t position);

#endif
