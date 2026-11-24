#include <math.h>
#include "view.h"

static const struct {
    const char *measure, *secondary;
    Series primary, extra;
    Color color, extra_color;
} metrics[SYSTEM_TABS] = {
    {"Total utilization", NULL, CPU_LOAD, 0, CHART_CPU, 0},
    {"GPU utilization", NULL, GPU_LOAD, 0, CHART_GPU, 0},
    {"Memory in use", NULL, RAM_LOAD, 0, CHART_RAM, 0},
    {"Read", "Write", DISK_READ, DISK_WRITE, CHART_READ, CHART_WRITE},
    {"Receive", "Send", NET_IN, NET_OUT, CHART_RX, CHART_TX},
};

static void label(View *v, const char *text, float size, Color color, bool mono)
{
    cui_text_line(v->ui, cui_string(text), size, v->colors[color], 0, (cui_text_style){.monospace = mono});
}

static void clock_face(View *v, GDateTime *now, int width, int height)
{
    g_autofree char *time = g_date_time_format(now, "%H:%M");
    g_autofree char *seconds = g_date_time_format(now, "%S");
    g_autofree char *weekday = g_date_time_format(now, "%A");
    g_autofree char *date = g_date_time_format(now, "%e %B %Y");
    float size = CLAMP(width / 6.0f, 20, 54);
    cui_context *ui = v->ui;
    cui_begin_box(ui, 0, (cui_box){.width = width, .height = height, .clip = true,
                                  .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER, .gap = 6});
    label(v, weekday, v->font_size, ACCENT, false);
    cui_begin_box(ui, 0, (cui_box){.row = true, .gap = 5, .align_y = CUI_ALIGN_END});
    label(v, time, size, FG, true);
    label(v, seconds, size * 0.38f, MUTED, true);
    cui_end_box(ui);
    label(v, g_strstrip(date), v->font_size, MUTED, false);
    cui_end_box(ui);
}

void dashboard_clock(View *v, int height)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    int width = v->width - 2 * v->inset;
    bool wide = width >= 470;
    int face_width = wide ? width * 44 / 100 : width;
    int face_height = wide ? height : MIN(height, 124);
    cui_context *ui = v->ui;
    cui_begin_box(ui, 0, (cui_box){.width = width, .height = height, .row = wide,
        .radius = 12, .clip = true, .align_y = CUI_ALIGN_CENTER, .background = v->colors[SELECT_BG]});
    clock_face(v, now, face_width, face_height);
    int calendar_width = wide ? width - face_width - 1 : width;
    int calendar_height = wide ? height : height - face_height - 1;
    if (calendar_height >= 125 && calendar_width >= 150) {
        cui_begin_box(ui, 0, (cui_box){.width = wide ? 1 : width, .height = wide ? MIN(180, height) : 1,
                                      .background = v->colors[BORDER]});
        cui_end_box(ui);
        GDate date = {0};
        g_date_set_dmy(&date, g_date_time_get_day_of_month(now), g_date_time_get_month(now),
                       g_date_time_get_year(now));
        calendar_month(v, &date, &date, calendar_width, calendar_height, false, true);
    }
    cui_end_box(ui);
}

static void quantity(char *out, size_t size, double value, bool rate)
{
    if (!isfinite(value)) {
        g_strlcpy(out, "—", size);
        return;
    }
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    unsigned unit = 0;
    while (value >= 1024 && unit + 1 < G_N_ELEMENTS(units)) {
        value /= 1024;
        unit++;
    }
    g_snprintf(out, size, "%.*f %s%s", (value < 100 && unit) ? 1 : 0,
               value, units[unit], rate ? "/s" : "");
}

static void sample_label(char *out, size_t size, const System *s, SystemTab tab, double value)
{
    if (tab >= DISK)
        quantity(out, size, value, true);
    else if (!isfinite(value))
        g_strlcpy(out, "—", size);
    else
        g_snprintf(out, size, "%.0f %s", value, tab == GPU && s->gpu_frequency ? "MHz" : "%");
}

static double chart_max(const System *s, SystemTab tab)
{
    if (tab <= RAM && !(tab == GPU && s->gpu_frequency))
        return 100;
    double peak = tab == GPU ? 100 : 1024;
    for (size_t i = 0; i < s->count; i++) {
        peak = fmax(peak, system_point(s, metrics[tab].primary, i));
        if (metrics[tab].secondary)
            peak = fmax(peak, system_point(s, metrics[tab].extra, i));
    }
    if (tab >= DISK)
        return exp2(ceil(log2(peak)));
    double step = pow(10, floor(log10(peak)));
    const double multiples[] = {1, 2, 4, 8, 10};
    for (size_t i = 0; i < G_N_ELEMENTS(multiples); i++)
        if (multiples[i] * step >= peak)
            return multiples[i] * step;
    return 10 * step;
}

static bool chart_has_data(const System *s, SystemTab tab)
{
    for (size_t i = 0; i < s->count; i++)
        if (isfinite(system_point(s, metrics[tab].primary, i)) ||
            (metrics[tab].secondary && isfinite(system_point(s, metrics[tab].extra, i))))
            return true;
    return false;
}

static void value_block(View *v, const System *s, SystemTab tab, bool secondary, float size)
{
    Series series = secondary ? metrics[tab].extra : metrics[tab].primary;
    const char *name = secondary ? metrics[tab].secondary : metrics[tab].measure;
    if (tab == GPU && s->gpu_frequency)
        name = "Core frequency";
    char value[32];
    sample_label(value, sizeof value, s, tab, system_point(s, series, s->count ? s->count - 1 : 0));
    cui_begin_box(v->ui, 0, (cui_box){.width = -1, .gap = 2, .clip = true});
    label(v, name, MIN(13, v->font_size), MUTED, false);
    label(v, value, size, secondary ? metrics[tab].extra_color : metrics[tab].color, true);
    cui_end_box(v->ui);
}

static void detail(View *v, const System *s, SystemTab tab)
{
    char note[96] = {0};
    if (tab == CPU) {
        int threads = g_get_num_processors();
        g_snprintf(note, sizeof note, "%d logical %s", threads, threads == 1 ? "CPU" : "CPUs");
    } else if (tab == GPU) {
        const char *gpu_note = s->gpu_frequency ? "Utilization unavailable · frequency shown" :
            s->gpu_fd >= 0 ? "Intel render engine" : s->gpu_path ? "GPU busy" : "GPU metrics unavailable";
        g_strlcpy(note, gpu_note, sizeof note);
    } else if (tab == NETWORK) {
        g_strlcpy(note, "All interfaces except loopback", sizeof note);
    } else {
        uint64_t used = tab == RAM ? s->memory_used : s->disk_used;
        uint64_t total = tab == RAM ? s->memory_total : s->disk_total;
        if (total) {
            char used_text[24], total_text[24];
            quantity(used_text, sizeof used_text, (double)used, false);
            quantity(total_text, sizeof total_text, (double)total, false);
            g_snprintf(note, sizeof note, "%s of %s used%s", used_text, total_text, tab == DISK ? " on /" : "");
        }
    }
    label(v, note, MIN(12, v->font_size), MUTED, false);
}

void dashboard_system(View *v, const System *s, SystemTab tab, int height)
{
    int width = v->width - 2 * v->inset;
    const char *const labels[] = {"CPU", "GPU", "RAM", "Disk", width < 335 ? "Net" : "Network"};
    const Color colors[] = {CHART_CPU, CHART_GPU, CHART_RAM, CHART_READ, CHART_RX};
    view_tabs(v, labels, colors, SYSTEM_TABS, tab, width);
    if (height < 103 || width < 110)
        return;
    cui_context *ui = v->ui;
    const int pad = width >= 260 ? 16 : 9;
    const int card_height = height - 49;
    const bool axes = width >= 280;
    cui_begin_box(ui, 0, (cui_box){.width = width, .height = 11});
    cui_end_box(ui);
    cui_begin_box(ui, 0, (cui_box){.width = width, .height = card_height,
        .padding = pad, .gap = 6, .radius = 12, .clip = true,
        .background = cui_color_mix_oklab(v->colors[BG], v->colors[SELECT_BG], 0.38f),
        .border = v->colors[BORDER], .border_width = 1});
    cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true, .gap = 10});
    float number_size = CLAMP((width - pad * 2) / (tab >= DISK ? 18.0f : 10.0f), 11, 31);
    int chart_height = card_height - 2 * pad - (int)ceilf(number_size * 1.3f) - 64;
    value_block(v, s, tab, false, number_size);
    if (metrics[tab].secondary)
        value_block(v, s, tab, true, number_size);
    cui_end_box(ui);
    detail(v, s, tab);
    if (chart_height >= 36) {
        cui_begin_box(ui, 0, (cui_box){.width = -1, .height = chart_height, .row = true, .gap = 9});
        cui_id id = cui_id_from(ui, "system-history");
        bool has_data = chart_has_data(s, tab);
        cui_begin_box(ui, id, (cui_box){.width = -1, .height = -1,
                                       .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER});
        if (has_data)
            cui_track_bounds(ui, id, &v->chart);
        else
            label(v, s->count < 2 ? "Sampling…" : "No data available", MIN(13, v->font_size), MUTED, false);
        cui_end_box(ui);
        if (axes && has_data) {
            double max = chart_max(s, tab);
            cui_begin_box(ui, 0, (cui_box){.width = tab >= DISK ? 79 : 62, .height = -1});
            for (int i = 0; i < 3; i++) {
                char tick[32];
                sample_label(tick, sizeof tick, s, tab, max * (2 - i) / 2);
                cui_begin_absolute_box(ui, 0, (cui_box){0},
                    (cui_rect){0, roundf((chart_height - 15) * i / 2.0f), tab >= DISK ? 79 : 62, 15});
                label(v, tick, MIN(11, v->mono_size), MUTED, true);
                cui_end_box(ui);
            }
            cui_end_box(ui);
        }
        cui_end_box(ui);
        cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true,
            .custom_padding = true, .padding_edges = {0, axes && has_data ? (tab >= DISK ? 88 : 71) : 0, 0, 0}});
        cui_begin_box(ui, 0, (cui_box){.width = -1});
        label(v, "1 min ago", 11, MUTED, false);
        cui_end_box(ui);
        label(v, "Now", 11, MUTED, false);
        cui_end_box(ui);
    }
    cui_end_box(ui);
}

static void rgba(cairo_t *cr, cui_color c, double alpha)
{
    cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0, alpha);
}

static void plot_series(cairo_t *cr, const System *s, Series series, cui_color c,
                        double max, double x, double y, double width, double height)
{
    for (size_t i = 0; i < s->count;) {
        double value = system_point(s, series, i);
        if (!isfinite(value)) {
            i++;
            continue;
        }
        double start = x + width * system_position(s, i);
        double end = start, py = y + height * (1 - value / max);
        cairo_move_to(cr, start, py);
        for (i++; i < s->count; i++) {
            value = system_point(s, series, i);
            if (!isfinite(value))
                break;
            end = x + width * system_position(s, i);
            py = y + height * (1 - value / max);
            cairo_line_to(cr, end, py);
        }
        cairo_path_t *path = cairo_copy_path(cr);
        if (end > start) {
            cairo_line_to(cr, end, y + height);
            cairo_line_to(cr, start, y + height);
            cairo_close_path(cr);
            cairo_pattern_t *gradient = cairo_pattern_create_linear(0, y, 0, y + height);
            cairo_pattern_add_color_stop_rgba(gradient, 0, c.r / 255.0, c.g / 255.0, c.b / 255.0, 0.32);
            cairo_pattern_add_color_stop_rgba(gradient, 1, c.r / 255.0, c.g / 255.0, c.b / 255.0, 0.03);
            cairo_set_source(cr, gradient);
            cairo_fill(cr);
            cairo_pattern_destroy(gradient);
        }
        cairo_new_path(cr);
        cairo_append_path(cr, path);
        cairo_path_destroy(path);
        cairo_set_line_width(cr, 1.7);
        rgba(cr, c, 0.94);
        cairo_stroke(cr);
        if (i == s->count) {
            cairo_arc(cr, end, py, 2.5, 0, 2 * G_PI);
            cairo_fill(cr);
        }
    }
}

void dashboard_plot(const View *v, const System *s, SystemTab tab, cairo_t *cr)
{
    cui_rect r = v->chart;
    if (r.w < 18 || r.h < 15)
        return;
    double x = r.x + 4.5, y = r.y + 7.5, width = r.w - 9, height = r.h - 15;
    double max = chart_max(s, tab);
    cairo_save(cr);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_clip(cr);
    cairo_set_line_width(cr, 1);
    rgba(cr, v->colors[BORDER], 0.82);
    for (int i = 0; i <= 4; i++) {
        double line_y = floor(y + height * i / 4) + 0.5;
        cairo_move_to(cr, x, line_y);
        cairo_line_to(cr, x + width, line_y);
    }
    for (int i = 0; i <= 6; i++) {
        double line_x = floor(x + width * i / 6) + 0.5;
        cairo_move_to(cr, line_x, y);
        cairo_line_to(cr, line_x, y + height);
    }
    cairo_stroke(cr);
    if (metrics[tab].secondary)
        plot_series(cr, s, metrics[tab].extra, v->colors[metrics[tab].extra_color], max, x, y, width, height);
    plot_series(cr, s, metrics[tab].primary, v->colors[metrics[tab].color], max, x, y, width, height);
    cairo_restore(cr);
}
