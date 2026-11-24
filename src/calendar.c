#include <time.h>
#include "view.h"

static GDate local_today(void)
{
    GDate date = {0};
    g_date_set_time_t(&date, time(NULL));
    return date;
}

void calendar_go_today(Calendar *cal)
{
    cal->today = local_today();
    cal->focus = cal->today;
    cal->preferred_day = g_date_get_day(&cal->focus);
}

void calendar_open(Calendar *cal)
{
    *cal = (Calendar){.tab = CAL_MONTH};
    calendar_go_today(cal);
}

bool calendar_update_today(Calendar *cal)
{
    GDate now = local_today();
    if (g_date_compare(&now, &cal->today) == 0)
        return false;
    if (g_date_compare(&cal->focus, &cal->today) == 0) {
        cal->focus = now;
        cal->preferred_day = g_date_get_day(&now);
    }
    cal->today = now;
    return true;
}

static GDate offset_date(const GDate *date, int days)
{
    GDate last = {0}, result = {0};
    g_date_set_dmy(&last, 31, G_DATE_DECEMBER, 9999);
    int day = (int)g_date_get_julian(date) + days;
    if (day >= 1 && day <= (int)g_date_get_julian(&last))
        g_date_set_julian(&result, day);
    return result;
}

void calendar_shift(Calendar *cal, int direction)
{
    GDate *focus = &cal->focus;
    if (cal->tab == CAL_YEAR || cal->tab == CAL_MONTH) {
        int months = (g_date_get_year(focus) - 1) * 12 + g_date_get_month(focus) - 1;
        months += direction * (cal->tab == CAL_YEAR ? 12 : 1);
        if (months >= 0 && months < 9999 * 12) {
            int year = months / 12 + 1, month = months % 12 + 1;
            int day = MIN(cal->preferred_day, g_date_get_days_in_month(month, year));
            g_date_set_dmy(focus, day, month, year);
        }
    } else {
        int days = cal->tab == CAL_WEEK ? 7 : cal->tab == CAL_THREE_DAYS ? 3 : 1;
        GDate date = offset_date(focus, direction * days);
        if (g_date_valid(&date)) {
            *focus = date;
            cal->preferred_day = g_date_get_day(focus);
        }
    }
}

static char *format_date(const GDate *date, const char *format)
{
    g_autoptr(GDateTime) time = g_date_time_new_utc(g_date_get_year(date), g_date_get_month(date),
                                                   g_date_get_day(date), 12, 0, 0);
    return g_date_time_format(time, format);
}

static void label(View *v, const char *str, float size, Color color, bool mono, bool bold)
{
    cui_text_line(v->ui, cui_string(str), size, v->colors[color], 0,
                  (cui_text_style){.monospace = mono, .weight = bold ? CUI_TEXT_WEIGHT_BOLD : 0});
}

static bool same_month(const GDate *a, const GDate *b)
{
    return g_date_get_year(a) == g_date_get_year(b) && g_date_get_month(a) == g_date_get_month(b);
}

void calendar_month(View *v, const GDate *month, const GDate *today, int width, int height,
                    bool compact, bool heading)
{
    cui_context *ui = v->ui;
    GDate first = *month;
    g_date_set_day(&first, 1);
    int offset = g_date_get_weekday(&first) - G_DATE_MONDAY;
    int days = g_date_get_days_in_month(g_date_get_month(&first), g_date_get_year(&first));
    int today_number = same_month(&first, today) ? g_date_get_day(today) : 0;
    int gap = compact ? 1 : 4;
    int head_size = compact ? MIN(13, MAX(8, height / 8)) : MIN(20, (int)v->font_size);
    int weekday_h = compact ? MIN(17, MAX(9, height / 9)) : 21;
    int cell = CLAMP((width - (compact ? 4 : 16)) / 7, 1, compact ? 24 : 80);
    int row = CLAMP((height - weekday_h - (heading ? head_size + gap + 4 : 0) - gap - 2) / 6,
                    1, compact ? 22 : 38);
    float digits = MAX(1, MIN(v->mono_size, MIN(row - 2, cell * 0.60f)));
    cui_begin_box(ui, 0, (cui_box){.width = width, .height = height, .clip = true,
                                  .gap = gap, .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER});
    if (heading) {
        g_autofree char *name = format_date(&first, compact ? "%b" : "%B %Y");
        label(v, name, head_size, today_number && compact ? ACCENT : FG, false, compact);
    }
    cui_begin_box(ui, 0, (cui_box){.row = true, .height = weekday_h});
    GDate monday = {0};
    g_date_set_dmy(&monday, 1, G_DATE_JANUARY, 2024);
    for (int i = 0; i < 7; i++) {
        GDate date = offset_date(&monday, i);
        g_autofree char *day = format_date(&date, "%a");
        if (cell < 24)
            *g_utf8_next_char(day) = '\0';
        cui_begin_box(ui, 0, (cui_box){.width = cell, .height = -1, .align_x = CUI_ALIGN_CENTER,
                                      .align_y = CUI_ALIGN_CENTER, .clip = true});
        label(v, day, MAX(1, MIN(cell - 1, compact ? 10 : 12)), MUTED, false, false);
        cui_end_box(ui);
    }
    cui_end_box(ui);
    cui_begin_box(ui, 0, (cui_box){0});
    for (int week = 0; week < 6; week++) {
        cui_begin_box(ui, 0, (cui_box){.row = true, .height = row});
        for (int i = 0; i < 7; i++) {
            int day = week * 7 + i - offset + 1;
            cui_begin_box(ui, 0, (cui_box){.width = cell, .height = row, .align_x = CUI_ALIGN_CENTER,
                                          .align_y = CUI_ALIGN_CENTER});
            if (day >= 1 && day <= days) {
                char str[4];
                g_snprintf(str, sizeof str, "%d", day);
                bool current = day == today_number;
                int badge = MIN(cell, MIN(row, compact ? 21 : 29));
                cui_begin_box(ui, 0, (cui_box){.width = badge, .height = badge, .radius = 6,
                    .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER,
                    .background = current ? v->colors[ACCENT] : (cui_color){0}});
                label(v, str, digits, current ? BG : i >= 5 ? MUTED : FG, true, false);
                cui_end_box(ui);
            }
            cui_end_box(ui);
        }
        cui_end_box(ui);
    }
    cui_end_box(ui);
    cui_end_box(ui);
}

static int year_columns(int width)
{
    return width >= 550 ? 4 : width >= 320 ? 3 : 2;
}

int calendar_height(const Calendar *cal, int width)
{
    switch (cal->tab) {
    case CAL_YEAR: return year_columns(width) == 4 ? 504 : year_columns(width) == 3 ? 542 : 584;
    case CAL_MONTH: return 354;
    case CAL_WEEK: case CAL_THREE_DAYS: return 233;
    case CAL_DAY: default: return 268;
    }
}

static cui_color card_color(const View *v)
{
    return cui_color_mix_oklab(v->colors[BG], v->colors[SELECT_BG], 0.44f);
}

static void year_grid(View *v, const Calendar *cal, int width, int height)
{
    int cols = year_columns(width), rows = 12 / cols, gap = 7;
    if (width < 140)
        gap = 3;
    int row = MAX(1, (height - (rows - 1) * gap) / rows);
    int room = MAX(cols, width - (cols - 1) * gap);
    int year = g_date_get_year(&cal->focus);
    for (int r = 0; r < rows; r++) {
        cui_begin_box(v->ui, 0, (cui_box){.row = true, .width = -1, .height = row, .gap = gap});
        for (int i = 0; i < cols; i++) {
            int cell = (i + 1) * room / cols - i * room / cols;
            GDate month = {0};
            g_date_set_dmy(&month, 1, r * cols + i + 1, year);
            cui_begin_box(v->ui, 0, (cui_box){.width = cell, .height = row, .radius = 8,
                .background = card_color(v), .border_width = 1,
                .border = v->colors[same_month(&month, &cal->today) ? ACCENT : BORDER]});
            calendar_month(v, &month, &cal->today, cell, row, true, true);
            cui_end_box(v->ui);
        }
        cui_end_box(v->ui);
    }
}

static GDate interval_start(const Calendar *cal)
{
    if (cal->tab == CAL_WEEK)
        return offset_date(&cal->focus, G_DATE_MONDAY - g_date_get_weekday(&cal->focus));
    return cal->focus;
}

static char *period_title(const Calendar *cal)
{
    if (cal->tab == CAL_YEAR)
        return format_date(&cal->focus, "%Y");
    if (cal->tab == CAL_MONTH || cal->tab == CAL_DAY)
        return format_date(&cal->focus, "%B %Y");
    GDate start = interval_start(cal);
    GDate end = offset_date(&start, cal->tab == CAL_WEEK ? 6 : 2);
    if (!g_date_valid(&end))
        g_date_set_dmy(&end, 31, G_DATE_DECEMBER, 9999);
    bool same_year = g_date_get_year(&start) == g_date_get_year(&end);
    g_autofree char *a = format_date(&start, same_month(&start, &end) ? "%e"
                                          : same_year ? "%e %b" : "%e %b %Y");
    g_autofree char *b = format_date(&end, same_month(&start, &end) ? "%e %B %Y" : "%e %b %Y");
    return g_strconcat(g_strstrip(a), " – ", g_strstrip(b), NULL);
}

static void period_bar(View *v, const Calendar *cal, int width, int height)
{
    g_autofree char *title = period_title(cal);
    cui_context *ui = v->ui;
    cui_begin_box(ui, 0, (cui_box){.width = width, .height = height, .row = true, .gap = 8,
                                   .align_y = CUI_ALIGN_CENTER, .clip = true});
    cui_begin_box(ui, 0, (cui_box){.width = -1, .clip = true});
    label(v, title, MIN(width < 320 ? 15 : 20, v->font_size + 3), FG, false, true);
    cui_end_box(ui);
    if (width >= 166) {
        cui_id id = cui_id_from(ui, "calendar-today");
        cui_begin_box(ui, id, (cui_box){.width = 58, .height = MIN(28, height), .radius = 6,
            .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER,
            .background = v->colors[SELECT_BG]});
        cui_track_bounds(ui, id, &v->today_button);
        label(v, "Today", MIN(13, v->font_size), ACCENT, false, false);
        cui_end_box(ui);
    }
    cui_end_box(ui);
}

static void day_cards(View *v, const Calendar *cal, int width, int height)
{
    int count = cal->tab == CAL_WEEK ? 7 : 3, gap = width < 335 ? 3 : 6;
    int room = MAX(count, width - (count - 1) * gap);
    GDate start = interval_start(cal);
    cui_context *ui = v->ui;
    cui_begin_box(ui, 0, (cui_box){.row = true, .width = width, .height = height, .gap = gap, .clip = true});
    for (int i = 0; i < count; i++) {
        int cell = (i + 1) * room / count - i * room / count;
        GDate date = offset_date(&start, i);
        bool valid = g_date_valid(&date);
        bool today = valid && g_date_compare(&date, &cal->today) == 0;
        cui_begin_box(ui, 0, (cui_box){.width = cell, .height = -1, .radius = 9, .gap = 5,
            .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER, .clip = true,
            .background = today ? v->colors[SELECT_BG] : card_color(v),
            .border_width = 1, .border = v->colors[today ? ACCENT : BORDER]});
        if (valid) {
            g_autofree char *weekday = format_date(&date, cell >= 100 ? "%A" : "%a");
            g_autofree char *number = format_date(&date, "%e");
            g_autofree char *month = format_date(&date, "%b");
            label(v, weekday, CLAMP(cell / 5.0f, 6, 13), today ? ACCENT : MUTED, false, false);
            label(v, g_strstrip(number), CLAMP(cell * 0.42f, 8, 48), FG, true, false);
            label(v, month, CLAMP(cell / 5.0f, 6, 13), MUTED, false, false);
        }
        cui_end_box(ui);
    }
    cui_end_box(ui);
}

static void one_day(View *v, const Calendar *cal, int width, int height)
{
    g_autofree char *day = format_date(&cal->focus, "%e");
    g_autofree char *weekday = format_date(&cal->focus, "%A");
    bool today = g_date_compare(&cal->focus, &cal->today) == 0;
    int year = g_date_get_year(&cal->focus);
    char week[24], ordinal[32];
    g_snprintf(week, sizeof week, "Week %u", g_date_get_iso8601_week_of_year(&cal->focus));
    g_snprintf(ordinal, sizeof ordinal, "Day %u of %d", g_date_get_day_of_year(&cal->focus),
               g_date_is_leap_year(year) ? 366 : 365);
    cui_context *ui = v->ui;
    bool wide = width >= 320;
    cui_begin_box(ui, 0, (cui_box){.width = width, .height = height, .row = wide, .gap = wide ? 24 : 4,
        .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER, .clip = true,
        .radius = 10, .border_width = 1, .border = v->colors[BORDER], .background = card_color(v)});
    label(v, g_strstrip(day), CLAMP(height * (wide ? 0.49f : 0.32f), 15, 85), today ? ACCENT : FG, true, false);
    cui_begin_box(ui, 0, (cui_box){.gap = 6, .align_x = wide ? CUI_ALIGN_START : CUI_ALIGN_CENTER});
    label(v, weekday, MIN(v->font_size + 6, 22), FG, false, true);
    label(v, week, MIN(v->mono_size, 13), MUTED, true, false);
    label(v, ordinal, MIN(v->mono_size, 13), MUTED, true, false);
    cui_end_box(ui);
    cui_end_box(ui);
}

void calendar_draw(View *v, const Calendar *cal, int height)
{
    const char *const labels[] = {"Year", "Month", "Week", "3-Days", "Day"};
    int width = v->width - 2 * v->inset;
    view_tabs(v, labels, NULL, CALENDAR_TABS, cal->tab, width);
    if (height < 93)
        return;
    period_bar(v, cal, width, 50);
    height -= 88;
    switch (cal->tab) {
    case CAL_YEAR:
        cui_begin_box(v->ui, 0, (cui_box){.width = width, .height = height, .gap = width < 140 ? 3 : 7, .clip = true});
        year_grid(v, cal, width, height);
        cui_end_box(v->ui);
        break;
    case CAL_MONTH:
        cui_begin_box(v->ui, 0, (cui_box){.width = width, .height = height,
            .radius = 10, .border_width = 1, .border = v->colors[BORDER], .background = card_color(v)});
        calendar_month(v, &cal->focus, &cal->today, width, height, false, false);
        cui_end_box(v->ui);
        break;
    case CAL_WEEK: case CAL_THREE_DAYS:
        day_cards(v, cal, width, height);
        break;
    case CAL_DAY:
        one_day(v, cal, width, height);
        break;
    default:
        break;
    }
}
