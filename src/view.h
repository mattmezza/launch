#ifndef LAUNCH_VIEW_H
#define LAUNCH_VIEW_H

#include <cairo.h>
#include <pango/pangocairo.h>
#include "launch.h"
#include "calendar.h"

enum { MAX_VISIBLE = 512, VIEW_TABS = MAX((int)SYSTEM_TABS, (int)CALENDAR_TABS) };

typedef struct {
    cui_rect bounds;
    size_t rank;
} Hit;

typedef struct {
    cui_context *ui;
    PangoLayout *layout;
    PangoFontDescription *normal, *mono;
    float font_size, mono_size;
    int width, height, row, header, footer, inset, radius;
    int panel_width, panel_height, border_width;
    bool bar, horizontal, caret_visible;
    size_t visible, hit_count;
    Hit hits[MAX_VISIBLE];
    cui_rect chart, tabs[VIEW_TABS], today_button;
    cui_color colors[COLORS];
} View;

bool view_init(View *, const Settings *, Display *, Colormap);
void view_clear(View *);
void view_fit(View *, Model *, const Settings *, const Calendar *, int max_width, int max_height);
void view_draw(View *, const Model *, const Settings *, const System *, SystemTab,
               const Calendar *, cairo_surface_t *);
size_t view_previous_page(View *, const Model *, const Settings *);
size_t view_hit(const View *, int x, int y);
int view_tab(const View *, int x, int y);
bool view_contains(cui_rect, int x, int y);
void view_tabs(View *, const char *const labels[], const Color colors[], int count, int active, int width);

void dashboard_clock(View *, int height);
void dashboard_system(View *, const System *, SystemTab, int height);
void dashboard_plot(const View *, const System *, SystemTab, cairo_t *);
int calendar_height(const Calendar *, int width);
void calendar_draw(View *, const Calendar *, int height);
void calendar_month(View *, const GDate *month, const GDate *today, int width, int height, bool compact, bool heading);
void glow_draw(const View *, cairo_surface_t *dest, cairo_surface_t *base, double distance);

#endif
