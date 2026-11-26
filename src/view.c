#include <X11/Xutil.h>
#include <cairo-xlib.h>
#include <fontconfig/fontconfig.h>
#include <math.h>
#include <string.h>
#include "view.h"
#include "icons.h"

typedef enum { SEARCH_ICON, EMOJI_ICON, CALC_ICON, APP_ICON, COMMAND_ICON, ENTER_ICON,
               EMPTY_RESULTS_ICON, SYSTEM_ICON, CALENDAR_ICON, FILE_ICON, FOLDER_ICON,
               WEB_ICON, ICON_COUNT } Icon;

static const struct {
    const unsigned char *mask;
    int size;
} icons[ICON_COUNT] = {
    {icon_magnifying_glass, 20}, {icon_face_smile, 16}, {icon_calculator, 16},
    {icon_squares_2x2, 16}, {icon_command_line, 16}, {icon_arrow_turn_down_left, 15},
    {icon_empty_results, 24}, {icon_system, 16}, {icon_calendar, 16},
    {icon_file, 16}, {icon_folder, 16}, {icon_web, 16},
};
static unsigned char pixels[ICON_COUNT][24 * 24 * 4];

static PangoFontDescription *font_description(const char *name, int fallback)
{
    PangoFontDescription *desc;
    const char *dash = strrchr(name, '-');
    if (strchr(name, ':') || (dash && g_ascii_isdigit(dash[1]))) {
        FcPattern *pattern = FcNameParse((const FcChar8 *)name);
        FcChar8 *family;
        double size;
        int weight, slant;
        desc = pango_font_description_new();
        if (pattern) {
            if (FcPatternGetString(pattern, FC_FAMILY, 0, &family) == FcResultMatch)
                pango_font_description_set_family(desc, (const char *)family);
            if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &size) == FcResultMatch &&
                size > 0 && size <= 256)
                pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
            else if (FcPatternGetDouble(pattern, FC_SIZE, 0, &size) == FcResultMatch &&
                     size > 0 && size <= 192)
                pango_font_description_set_absolute_size(desc, size * 96 / 72 * PANGO_SCALE);
            if (FcPatternGetInteger(pattern, FC_WEIGHT, 0, &weight) == FcResultMatch &&
                weight >= FC_WEIGHT_DEMIBOLD)
                pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
            if (FcPatternGetInteger(pattern, FC_SLANT, 0, &slant) == FcResultMatch &&
                slant != FC_SLANT_ROMAN)
                pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
            FcPatternDestroy(pattern);
        }
    } else {
        desc = pango_font_description_from_string(name);
    }
    int size = pango_font_description_get_size(desc);
    if (size <= 0 || size > 256 * PANGO_SCALE)
        size = fallback * PANGO_SCALE;
    pango_font_description_set_absolute_size(desc, size);
    if (!pango_font_description_get_family(desc))
        pango_font_description_set_family(desc, "sans-serif");
    return desc;
}

static void layout_text(View *v, cui_str text, float size, cui_text_style style)
{
    PangoFontDescription *font = pango_font_description_copy(style.monospace ? v->mono : v->normal);
    if (style.family == 1)
        pango_font_description_set_family(font, "Noto Color Emoji");
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    if (style.weight == CUI_TEXT_WEIGHT_BOLD)
        pango_font_description_set_weight(font, PANGO_WEIGHT_SEMIBOLD);
    pango_layout_set_font_description(v->layout, font);
    pango_font_description_free(font);
    pango_layout_set_attributes(v->layout, NULL);
    pango_layout_set_text(v->layout, text.data ? text.data : "", (int)text.length);
}

static cui_size measure(void *user, cui_str text, float size, cui_text_style style)
{
    View *v = user;
    layout_text(v, text, size, style);
    int width, height;
    pango_layout_get_size(v->layout, &width, &height);
    return (cui_size){(float)width / PANGO_SCALE, (float)height / PANGO_SCALE};
}

bool view_init(View *v, const Settings *s, Display *display, Colormap colormap)
{
    for (int i = 0; i < COLORS; i++) {
        const char *name = s->color[i];
        if (!name)
            continue;
        char full[8];
        if (*name == '#' && strlen(name) == 4) {
            g_snprintf(full, sizeof full, "#%c%c%c%c%c%c",
                       name[1], name[1], name[2], name[2], name[3], name[3]);
            name = full;
        }
        XColor color;
        if (!XParseColor(display, colormap, name, &color)) {
            fprintf(stderr, "launch: invalid X11 color '%s'\n", s->color[i]);
            return false;
        }
        v->colors[i] = (cui_color){color.red >> 8, color.green >> 8, color.blue >> 8, 255};
    }
    cui_color bg = v->colors[BG], fg = v->colors[FG];
    if (!s->color[MUTED])
        v->colors[MUTED] = cui_color_mix_oklab(bg, fg, 0.72f);
    if (!s->color[SELECT_BG])
        v->colors[SELECT_BG] = cui_color_mix_oklab(bg, v->colors[ACCENT], 0.28f);
    if (!s->color[SELECT_FG])
        v->colors[SELECT_FG] = fg;
    if (!s->color[BORDER])
        v->colors[BORDER] = cui_color_mix_oklab(bg, fg, 0.19f);
    v->caret_visible = true;
    v->normal = font_description(s->font, 15);
    v->mono = font_description(s->mono, 13);
    v->font_size = (float)pango_font_description_get_size(v->normal) / PANGO_SCALE;
    v->mono_size = (float)pango_font_description_get_size(v->mono) / PANGO_SCALE;
    PangoContext *context = pango_font_map_create_context(pango_cairo_font_map_get_default());
    int screen = DefaultScreen(display);
    cairo_surface_t *native = cairo_xlib_surface_create(display, RootWindow(display, screen),
                                                       DefaultVisual(display, screen), 1, 1);
    cairo_font_options_t *options = cairo_font_options_create();
    cairo_surface_get_font_options(native, options);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_ON);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_FULL);
    pango_cairo_context_set_font_options(context, options);
    pango_context_set_round_glyph_positions(context, TRUE);
    cairo_font_options_destroy(options);
    cairo_surface_destroy(native);
    v->layout = pango_layout_new(context);
    pango_layout_set_single_paragraph_mode(v->layout, TRUE);
    g_object_unref(context);
    for (size_t i = 0; i < ICON_COUNT; i++) {
        int size = icons[i].size;
        int stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, size);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                unsigned char *pixel = pixels[i] + (y * size + x) * 4;
                memset(pixel, 255, 3);
                pixel[3] = icons[i].mask[y * stride + x];
            }
    }
    v->ui = cui_create((cui_config){.width = 640, .height = 480, .max_elements = 8192,
                                    .frame_bytes = 1048576,
                                    .measure_styled = measure, .measure_user = v});
    return v->ui != NULL;
}

void view_clear(View *v)
{
    if (v->ui)
        cui_destroy(v->ui);
    if (v->normal)
        pango_font_description_free(v->normal);
    if (v->mono)
        pango_font_description_free(v->mono);
    g_clear_object(&v->layout);
}

static int width_of(View *v, const char *label)
{
    return (int)ceilf(measure(v, cui_string(label), v->font_size, (cui_text_style){0}).width);
}

static int horizontal_width(View *v, const Item *item, int room)
{
    return MIN(room, width_of(v, item->label) + 20);
}

static int query_width(View *v, const Settings *s)
{
    return MIN(v->width / 2, v->width / 3 + (s->prompt ? width_of(v, s->prompt) + 10 : 0));
}

static size_t horizontal_page(View *v, Model *m, const Settings *s)
{
    int room = MAX(1, v->width - query_width(v, s) - 40);
    size_t start = m->first;
    for (;;) {
        int left = room;
        size_t rank = start;
        for (; rank - start < MAX_VISIBLE; rank++) {
            Item *item = model_item(m, rank);
            if (!item)
                break;
            int width = horizontal_width(v, item, room);
            if (width > left)
                break;
            left -= width;
        }
        if (rank > m->selected || !model_item(m, m->selected)) {
            m->first = start;
            return rank - start;
        }
        ++start;
    }
}

size_t view_previous_page(View *v, const Model *m, const Settings *s)
{
    if (!v->horizontal)
        return m->first > m->page ? m->first - m->page : 0;
    int room = MAX(1, v->width - query_width(v, s) - 40);
    int left = room;
    size_t rank = m->first;
    for (; rank > 0; rank--) {
        const Item *item = model_item(m, rank - 1);
        if (!item)
            break;
        left -= horizontal_width(v, item, room);
        if (left < 0)
            break;
    }
    return rank;
}

void view_fit(View *v, Model *m, const Settings *s, const Calendar *calendar,
              int max_width, int max_height)
{
    v->bar = s->placement != CENTER;
    v->horizontal = m->mode == PICK && s->rows == 0;
    v->panel_width = v->bar ? max_width : MIN(s->width, max_width);
    v->border_width = MIN(s->border_width, (MIN(v->panel_width, max_height) - 1) / 4);
    int frame = 2 * v->border_width;
    v->width = v->panel_width - frame;
    max_height -= frame;
    v->inset = v->bar ? 0 : MIN(14, v->width / 6);
    int natural = (int)ceilf(measure(v, (cui_str)CUI_STR("Mg"), v->font_size,
                                     (cui_text_style){0}).height);
    v->row = s->row_height ? s->row_height : MAX(s->compatible ? 30 : 42, natural + 12);
    const Item *item = model_item(m, 0);
    bool single_result = item && (item->action == ITEM_WEB || item->direct_path);
    if (m->mode == FILES || m->file_query || single_result)
        v->row = MAX(v->row, (int)ceilf(natural + MAX(13, v->font_size - 3) * 1.3f + 7));
    v->header = MIN(max_height, v->bar ? v->row : MAX(natural + 24, 54));
    v->footer = v->bar ? 0 : MIN(38, MAX(0, (max_height - v->header) / 3));
    m->columns = m->mode == EMOJI ? (size_t)CLAMP((v->width - 2 * v->inset) / 58, 1, 12) : 1;
    int rows = s->rows ? s->rows : (m->mode == EMOJI ? 5 : 7);
    if (single_result) {
        rows = 1;
    } else if (m->mode == FILES || m->file_query) {
        size_t visible = m->file_pending ? MAX(m->page, m->matches->len) : m->matches->len;
        rows = (int)MIN((size_t)rows, MAX(visible, 2u));
    } else if (m->mode == EMOJI) {
        v->row = MAX(v->row, 52);
        size_t result_rows = (m->matches->len + m->columns - 1) / m->columns;
        rows = MIN(rows, (int)CLAMP(result_rows, 1, 5));
    } else if (m->mode == PICK && s->compatible) {
        rows = MIN(rows, (int)MIN(m->items->len, (guint)MAX_VISIBLE));
    }
    int max_rows = MAX(0, (max_height - v->header - v->footer - (v->bar ? 0 : 9)) / v->row);
    rows = CLAMP(rows, 0, MIN(max_rows, (int)(MAX_VISIBLE / m->columns)));
    v->visible = (size_t)rows * m->columns;
    if (!mode_has_search(m->mode) || model_showcase(m, s)) {
        bool minimal = m->mode == PALETTE && s->startup == MINIMAL;
        int room = v->width - 2 * v->inset;
        int body = m->mode == SYSTEM ? 326 : m->mode == CALENDAR ? calendar_height(calendar, room)
                                                                : room < 470 ? 351 : 248;
        if (minimal || max_height <= v->header + v->footer + 9)
            v->footer = 0;
        v->visible = m->page = 0;
        v->height = minimal ? v->header
            : MIN(max_height, v->header + body + v->footer + (v->bar ? 0 : 9));
    } else if (v->horizontal) {
        m->page = 0;
        if (m->selected < m->first)
            m->first = m->selected;
        v->visible = horizontal_page(v, m, s);
        m->page = v->visible;
        v->height = v->header;
    } else if (m->mode == CALCULATOR) {
        v->visible = 0;
        m->page = 0;
        v->height = MIN(max_height, v->header + MAX(v->row, 104) + v->footer + (v->bar ? 0 : 9));
    } else {
        m->page = v->visible;
        model_select(m, m->selected);
        v->height = MIN(max_height, v->header + rows * v->row + v->footer + (v->bar ? 0 : 9));
    }
    v->panel_height = v->height + frame;
    v->radius = MIN(s->corner_radius, MIN(v->panel_width, v->panel_height) / 2);
}

static void text(View *v, const char *label, float font, cui_color color, cui_text_style style)
{
    cui_text_line(v->ui, cui_string(label), font, color, 0, style);
}

void view_tabs(View *v, const char *const labels[], const Color colors[], int count,
               int active, int width)
{
    const int gap = width < 235 ? 2 : 4;
    cui_context *ui = v->ui;
    cui_begin_box(ui, 0, (cui_box){.row = true, .width = width, .height = 38, .gap = gap,
        .padding = 3, .radius = 9, .clip = true,
        .background = cui_color_mix_oklab(v->colors[BG], v->colors[SELECT_BG], 0.52f)});
    int room = MAX(count, width - 6 - (count - 1) * gap);
    for (int i = 0; i < count; i++) {
        cui_id id = cui_id_from(ui, labels[i]);
        Color accent = colors ? colors[i] : ACCENT;
        bool selected = i == active;
        int cell = (i + 1) * room / count - i * room / count;
        cui_begin_box(ui, id, (cui_box){.width = cell, .height = -1,
            .radius = 6, .clip = true, .row = true, .gap = 7,
            .background = selected ? v->colors[SELECT_BG] : (cui_color){0},
            .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER});
        cui_track_bounds(ui, id, &v->tabs[i]);
        if (colors && cell >= 70) {
            cui_begin_box(ui, 0, (cui_box){.width = 6, .height = 6, .radius = 3,
                                          .background = v->colors[accent]});
            cui_end_box(ui);
        }
        text(v, labels[i], MAX(7, MIN(MIN(v->font_size, 14), cell / 4.0f)),
             v->colors[selected ? accent : MUTED], (cui_text_style){0});
        cui_end_box(ui);
    }
    cui_end_box(ui);
}

static void icon(View *v, Icon which, cui_color color)
{
    unsigned size = (unsigned)icons[which].size;
    cui_paint_bitmap(v->ui, 0, (cui_bitmap_paint){
        .bitmap = {pixels[which], size, size, size * 4, 0},
        .opacity = 1, .tint = color, .custom_tint = true,
    }, (cui_box){.width = (float)size, .height = (float)size});
}

static void field(View *v, const Model *m, float width, float height, const char *placeholder)
{
    cui_context *ui = v->ui;
    cui_text_style style = {.monospace = m->mode == CALCULATOR};
    float font = style.monospace ? v->font_size : v->font_size + (v->bar ? 0 : 2);
    float caret_x = 0;
    float caret_y = roundf((height - font) / 2);
    cui_begin_box(ui, 0, (cui_box){.width = width, .height = height, .clip = true,
                                  .align_y = CUI_ALIGN_CENTER});
    if (*m->query) {
        layout_text(v, cui_string(m->query), font, style);
        PangoRectangle cursor;
        pango_layout_get_cursor_pos(v->layout, (int)m->cursor, &cursor, NULL);
        float x = roundf((float)cursor.x / PANGO_SCALE);
        float scroll = MAX(0.0f, x - MAX(4.0f, width - 5));
        caret_x = x - scroll;
        size_t from, to;
        if (model_selection(m, &from, &to)) {
            int *ranges = NULL, count = 0;
            PangoLayoutLine *line = pango_layout_get_line_readonly(v->layout, 0);
            pango_layout_line_get_x_ranges(line, (int)from, (int)to, &ranges, &count);
            for (int i = 0; i < count; i++) {
                float left = roundf((float)ranges[i * 2] / PANGO_SCALE) - scroll;
                float right = roundf((float)ranges[i * 2 + 1] / PANGO_SCALE) - scroll;
                cui_begin_absolute_box(ui, 0, (cui_box){.background = v->colors[SELECT_BG]},
                                       (cui_rect){left, MAX(0, caret_y - 2), right - left, roundf(font) + 4});
                cui_end_box(ui);
            }
            g_free(ranges);
        }
        cui_begin_box(ui, 0, (cui_box){.width = -1, .height = -1,
                                      .offset_x = -scroll, .align_y = CUI_ALIGN_CENTER});
        text(v, m->query, font, v->colors[FG], style);
        cui_end_box(ui);
    } else {
        cui_begin_box(ui, 0, (cui_box){.offset_x = 5});
        text(v, placeholder, font, v->colors[MUTED], style);
        cui_end_box(ui);
    }
    if (v->caret_visible) {
        cui_begin_absolute_box(ui, 0, (cui_box){.background = v->colors[ACCENT]},
                               (cui_rect){caret_x, caret_y, 1, roundf(font)});
        cui_end_box(ui);
    }
    cui_end_box(ui);
}

static const char *hint(Mode mode)
{
    switch (mode) {
    case EMOJI: return "Search emoji";
    case CALCULATOR: return "Enter an expression";
    case PICK: return "Filter items";
    case FILES: return "Search files and folders";
    case MWM: return "Search window manager actions";
    default: return "Search apps and workflows";
    }
}

static const char *scope(Mode mode)
{
    switch (mode) {
    case EMOJI: return "EMOJI";
    case CALCULATOR: return "CALC";
    case FILES: return "FILES";
    case MWM: return "MWM";
    default: return NULL;
    }
}

static void header(View *v, const Model *m, const Settings *s)
{
    const char *prompt = s->prompt;
    const char *tag = v->bar ? NULL : scope(m->file_query ? FILES : m->mode);
    int margin = v->bar ? 9 : v->inset + 6;
    int gap = v->bar ? 8 : 12;
    int limit = v->horizontal ? query_width(v, s) : v->width;
    int available = MAX(1, limit - 2 * margin);
    cui_context *ui = v->ui;
    cui_begin_box(ui, 0, (cui_box){.width = (float)limit, .height = (float)v->header,
                     .row = true, .gap = (float)gap, .align_y = CUI_ALIGN_CENTER, .clip = true,
                     .custom_padding = true, .padding_edges = {(float)margin, (float)margin, 0, 0}});
    if (prompt && *prompt) {
        int width = MIN(MAX(1, available / 2), width_of(v, prompt));
        cui_begin_box(ui, 0, (cui_box){.width = (float)width, .clip = true});
        text(v, prompt, v->font_size, v->colors[ACCENT], (cui_text_style){0});
        cui_end_box(ui);
        available -= width + gap;
    } else if (!v->bar && available >= 180) {
        icon(v, SEARCH_ICON, v->colors[ACCENT]);
        available -= 20 + gap;
    }
    if (tag && available > 220) {
        int tag_width = m->mode == EMOJI ? 64 : 58;
        available -= tag_width + gap;
        field(v, m, (float)MAX(1, available), (float)v->header, hint(m->mode));
        cui_begin_box(ui, 0, (cui_box){.width = (float)tag_width, .height = 25, .radius = 6,
                  .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER,
                  .background = v->colors[SELECT_BG]});
        text(v, tag, MAX(1, v->mono_size - 1), v->colors[ACCENT], (cui_text_style){.monospace = true});
        cui_end_box(ui);
    } else {
        field(v, m, (float)MAX(1, available), (float)v->header, v->bar ? "" : hint(m->mode));
    }
    cui_end_box(ui);
}

static void highlight(View *v, const Model *m, const Item *item, cui_color fg)
{
    const char *label = item->label;
    const char *query = m->file_query ? m->file_query : m->query;
    cui_text_color_span spans[64];
    size_t count = 0;
    if (*query) {
        /* Keep offsets in the original label, including UTF-8 slice boundaries. */
        g_autofree char *needle = search_key(query);
        const char *next = needle;
        const char *run = NULL;
        for (const char *p = label; *p && count < G_N_ELEMENTS(spans); p = g_utf8_next_char(p)) {
            const char *end = g_utf8_next_char(p);
            g_autofree char *glyph = g_strndup(p, (gsize)(end - p));
            g_autofree char *folded = search_key(glyph);
            while (*next == ' ')
                next++;
            size_t length = strlen(folded);
            if (*next && length && g_str_has_prefix(next, folded)) {
                next += length;
                if (!run)
                    run = p;
                if (*end && *next)
                    continue;
                p = end;
                spans[count++] = (cui_text_color_span){(size_t)(run - label), (size_t)(p - label),
                                                       v->colors[ACCENT]};
                break;
            } else if (run) {
                spans[count++] = (cui_text_color_span){(size_t)(run - label), (size_t)(p - label),
                                                       v->colors[ACCENT]};
                run = NULL;
            }
        }
        while (*next == ' ')
            next++;
        if (*next)
            count = 0;
    }
    cui_str label_str = cui_string(label);
    if (!count || !cui_text_context_line_colored(v->ui,
            (cui_text_line_source){label_str, 0, label_str.length}, v->font_size, fg, 0,
            (cui_text_style){0}, spans, count))
        text(v, label, v->font_size, fg, (cui_text_style){0});
}

static void hit_start(View *v, size_t rank, cui_box box)
{
    char key[32];
    g_snprintf(key, sizeof key, "row-%zu", rank);
    cui_id id = cui_id_from(v->ui, key);
    cui_begin_box(v->ui, id, box);
    if (v->hit_count < MAX_VISIBLE) {
        Hit *hit = &v->hits[v->hit_count++];
        hit->rank = rank;
        cui_track_bounds(v->ui, id, &hit->bounds);
    }
}

static void item_row(View *v, const Model *m, const Item *item, size_t rank, int width)
{
    cui_context *ui = v->ui;
    bool selected = rank == m->selected;
    cui_color fg = v->colors[selected ? SELECT_FG : item->emitted ? OUT_FG : FG];
    cui_color bg = selected ? v->colors[SELECT_BG]
                           : item->emitted ? v->colors[OUT_BG] : (cui_color){0};
    bool rich = (!v->bar || item->detail) && width >= 280;
    int inset = rich ? 10 : 9, gap = 10;
    hit_start(v, rank, (cui_box){.width = (float)width,
                .height = (float)(v->horizontal ? v->header : v->row), .row = true,
                .gap = (float)gap, .radius = rich ? 8 : 0, .clip = true,
                .background = bg, .align_y = CUI_ALIGN_CENTER, .custom_padding = true,
                .padding_edges = {(float)inset, (float)inset, 0, 0}});

    int note_width = rich && item->note ? 68 : 0;
    if (item->action == ITEM_WEB && width >= 200)
        note_width = CLAMP(9 * (int)strlen(item->note), 68, MAX(68, width / 3));
    if (note_width) {
        Icon which = item->action == ITEM_APP ? APP_ICON : item->action == ITEM_EMOJI
            ? EMOJI_ICON : item->action == ITEM_CALCULATOR ? CALC_ICON :
              item->action == ITEM_SYSTEM ? SYSTEM_ICON :
              item->action == ITEM_CALENDAR ? CALENDAR_ICON :
              item->action == ITEM_FILE ? FILE_ICON :
              item->action == ITEM_FILES || item->action == ITEM_FOLDER ? FOLDER_ICON :
              item->action == ITEM_WEB ? WEB_ICON : COMMAND_ICON;
        cui_begin_box(ui, 0, (cui_box){.width = 27, .height = 27, .radius = 6,
                       .background = selected ? v->colors[BG] : v->colors[SELECT_BG],
                       .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER});
        icon(v, which, v->colors[ACCENT]);
        cui_end_box(ui);
    }
    int label_width = MAX(1, width - 2 * inset - (note_width ? note_width + 27 + 2 * gap : 0));
    cui_begin_box(ui, 0, (cui_box){.width = (float)label_width, .gap = 1, .clip = true});
    if (v->bar || item->emitted || item->action == ITEM_WEB)
        text(v, item->label, v->font_size, fg, (cui_text_style){0});
    else
        highlight(v, m, item, fg);
    if (item->detail) {
        const char *detail = !v->footer && selected && m->error ? m->error : item->detail;
        text(v, detail, MAX(10, v->font_size - 4), v->colors[MUTED], (cui_text_style){0});
    }
    cui_end_box(ui);
    if (note_width) {
        cui_begin_box(ui, 0, (cui_box){.width = (float)note_width, .align_x = CUI_ALIGN_END, .clip = true});
        text(v, item->note, MAX(1, v->mono_size - 1),
             v->colors[item->action == ITEM_WEB ? ACCENT : MUTED], (cui_text_style){.monospace = true});
        cui_end_box(ui);
    }
    cui_end_box(ui);
}

static void horizontal(View *v, const Model *m, const Settings *s)
{
    cui_context *ui = v->ui;
    cui_begin_box(ui, 0, (cui_box){.width = -1, .height = -1, .row = true,
                                  .align_y = CUI_ALIGN_CENTER, .clip = true});
    header(v, m, s);
    cui_begin_box(ui, 0, (cui_box){.width = 20, .align_x = CUI_ALIGN_CENTER});
    if (m->first)
        text(v, "‹", v->font_size, v->colors[MUTED], (cui_text_style){0});
    cui_end_box(ui);
    int room = MAX(1, v->width - query_width(v, s) - 40);
    for (size_t i = 0; i < v->visible; i++) {
        size_t rank = m->first + i;
        Item *item = model_item(m, rank);
        item_row(v, m, item, rank, horizontal_width(v, item, room));
    }
    if (model_item(m, m->first + v->visible)) {
        cui_begin_box(ui, 0, (cui_box){.width = -1, .align_x = CUI_ALIGN_END,
                                       .custom_padding = true, .padding_edges = {0, 8, 0, 0}});
        text(v, "›", v->font_size, v->colors[MUTED], (cui_text_style){0});
        cui_end_box(ui);
    }
    cui_end_box(ui);
}

static void grid(View *v, const Model *m)
{
    const float gap = 5;
    float width = MAX(1.0f, (v->width - 2 * v->inset - gap * (m->columns - 1)) / m->columns);
    for (size_t i = 0; i < v->visible; i += m->columns) {
        cui_begin_box(v->ui, 0, (cui_box){.width = -1, .height = (float)v->row,
                                         .row = true, .gap = gap});
        for (size_t j = 0; j < m->columns; j++) {
            size_t rank = m->first + i + j;
            Item *item = model_item(m, rank);
            if (!item)
                break;
            bool selected = rank == m->selected;
            hit_start(v, rank, (cui_box){.width = width, .height = (float)v->row - 5,
                      .background = selected ? v->colors[SELECT_BG] : (cui_color){0},
                      .border = v->colors[ACCENT], .border_width = selected ? 1 : 0,
                      .radius = 9, .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER,
                      .clip = true});
            text(v, item->value, CLAMP(width - 2, 1, 30), v->colors[FG], (cui_text_style){.family = 1});
            cui_end_box(v->ui);
        }
        cui_end_box(v->ui);
    }
}

static void results(View *v, const Model *m)
{
    cui_context *ui = v->ui;
    float height = m->mode == CALCULATOR ? v->height - v->header - v->footer - (v->bar ? 0 : 9)
                                         : v->row * (float)(v->visible / m->columns);
    if (height <= 0)
        return;
    cui_begin_box(ui, 0, (cui_box){.width = -1, .height = height, .clip = true,
                                   .custom_padding = true,
                                   .padding_edges = {(float)v->inset, (float)v->inset, 0, 0}});
    if (m->mode == CALCULATOR) {
        bool valid = *m->result;
        const char *value = valid ? m->result : (*m->query ? "Incomplete or invalid expression" : "Try (18 + 6) / 4");
        cui_begin_box(ui, 0, (cui_box){.width = -1, .height = -1, .radius = 10, .padding = 15,
                     .background = v->colors[SELECT_BG], .gap = 6, .align_y = CUI_ALIGN_CENTER,
                     .clip = true});
        text(v, "RESULT", MAX(1, v->mono_size - 1), v->colors[ACCENT], (cui_text_style){.monospace = true});
        text(v, value, valid ? 32 : v->font_size, v->colors[valid ? FG : MUTED],
             (cui_text_style){.monospace = valid});
        cui_end_box(ui);
    } else if (!m->matches->len && (!v->bar || m->file_query || m->mode == FILES || m->error)) {
        cui_begin_box(ui, 0, (cui_box){.width = -1, .height = -1, .align_x = CUI_ALIGN_CENTER,
                                      .align_y = CUI_ALIGN_CENTER,
                                      .gap = height >= 86 ? 11 : 4});
        if (height >= 52 && v->width >= 70) {
            float size = height >= 86 ? 46 : 28;
            cui_begin_box(ui, 0, (cui_box){.width = size, .height = size, .radius = size / 2,
                          .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER,
                          .background = cui_color_mix_oklab(v->colors[BG], v->colors[SELECT_BG], 0.66f),
                          .border = cui_color_mix_oklab(v->colors[BG], v->colors[BORDER], 0.70f),
                .border_width = 1});
            Icon which = m->file_pending ? SEARCH_ICON :
                         m->mode == FILES && !m->file_query ? FOLDER_ICON : EMPTY_RESULTS_ICON;
            icon(v, which, v->colors[MUTED]);
            cui_end_box(ui);
        }
        const char *message = m->error ? m->error : m->file_pending ? "Searching files…" :
            m->mode == FILES && !m->file_query ? "Type a filename or folder path" : "No matches";
        text(v, message, v->font_size, v->colors[MUTED], (cui_text_style){0});
        cui_end_box(ui);
    } else if (m->mode == EMOJI) {
        grid(v, m);
    } else {
        for (size_t i = 0; i < v->visible; i++) {
            size_t rank = m->first + i;
            Item *item = model_item(m, rank);
            if (!item)
                break;
            item_row(v, m, item, rank, v->width - 2 * v->inset);
        }
    }
    cui_end_box(ui);
}

static void workflow_header(View *v, Mode mode, const System *system)
{
    cui_context *ui = v->ui;
    float inset = v->inset + 6;
    cui_begin_box(ui, 0, (cui_box){.width = -1, .height = v->header, .row = true,
        .gap = 7, .align_y = CUI_ALIGN_CENTER, .clip = true, .custom_padding = true,
        .padding_edges = {inset, inset, 0, 0}});
    cui_begin_box(ui, 0, (cui_box){.width = -1, .clip = true});
    text(v, mode == SYSTEM ? "System" : "Calendar", v->font_size + 4, v->colors[FG],
         (cui_text_style){.weight = CUI_TEXT_WEIGHT_BOLD});
    cui_end_box(ui);
    if (mode == SYSTEM && v->width >= 190) {
        int ms = system->interval_ms;
        char interval[24];
        if (ms >= 1000 && ms % 1000 == 0)
            g_snprintf(interval, sizeof interval, "%d s", ms / 1000);
        else
            g_snprintf(interval, sizeof interval, "%d ms", ms);
        cui_begin_box(ui, 0, (cui_box){.width = 6, .height = 6, .radius = 3,
                                      .background = v->colors[CHART_RAM]});
        cui_end_box(ui);
        text(v, interval, MIN(13, v->mono_size), v->colors[MUTED], (cui_text_style){.monospace = true});
    }
    cui_end_box(ui);
}

static void footer(View *v, const Model *m, const Settings *s)
{
    if (!v->footer)
        return;
    cui_context *ui = v->ui;
    Item *selected = model_item(m, m->selected);
    const char *left = m->error && selected ? m->error : m->mode == EMOJI && selected
        ? selected->label : m->came_from_palette ? "Esc Back" : "Esc Close";
    bool file = selected && (selected->action == ITEM_FILE || selected->action == ITEM_FOLDER);
    const char *right = selected && selected->action == ITEM_WEB ? "Open tab" :
        selected && selected->direct_path && selected->action == ITEM_FILE && s->files_reveal ? "Reveal" :
        m->mode == FILES || file ? "Open" : m->mode == PALETTE ? (selected && selected->note &&
        strcmp(selected->note, "Workflow") == 0 ? "Open workflow" : "Open")
        : m->mode == MWM ? "Run" : m->mode == PICK || s->print ? "Select" : "Copy";
    cui_begin_box(ui, 0, (cui_box){.width = -1, .height = (float)v->footer, .row = true,
                                    .align_y = CUI_ALIGN_CENTER, .gap = 8, .clip = true,
                                    .custom_padding = true,
                                    .padding_edges = {v->inset + 5.0f, v->inset + 5.0f, 0, 0}});
    cui_begin_box(ui, 0, (cui_box){.width = -1, .clip = true});
    text(v, left, MAX(1, v->mono_size - 1), v->colors[MUTED], (cui_text_style){0});
    cui_end_box(ui);
    bool actionable = selected || (m->mode == PICK && !s->require_match) ||
                      (m->mode == CALCULATOR && *m->result);
    if (actionable && v->width > 160) {
        if (file && v->width >= 355)
            text(v, "Ctrl+↵ Copy path", MAX(1, v->mono_size - 1), v->colors[MUTED], (cui_text_style){0});
        icon(v, ENTER_ICON, v->colors[ACCENT]);
        text(v, right, MAX(1, v->mono_size - 1), v->colors[FG], (cui_text_style){0});
    } else if (!mode_has_search(m->mode) && v->width >= 270) {
        const char *keys = m->mode == SYSTEM ? "← →  View     ↑ ↓  Refresh"
                                             : "← →  View     ↑ ↓  Date     T  Today";
        text(v, keys, MIN(13, v->font_size), v->colors[MUTED], (cui_text_style){0});
    }
    cui_end_box(ui);
}

static void color(cairo_t *cr, cui_color c)
{
    cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0);
}

static void round_rect(cairo_t *cr, cui_rect r, double radius)
{
    double x = r.x, y = r.y, w = r.w, h = r.h;
    double a = CLAMP(radius, 0.0, MAX(0.0, MIN(w, h) / 2.0));
    if (a == 0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - a, y + a, a, -G_PI_2, 0);
    cairo_arc(cr, x + w - a, y + h - a, a, 0, G_PI_2);
    cairo_arc(cr, x + a, y + h - a, a, G_PI_2, G_PI);
    cairo_arc(cr, x + a, y + a, a, G_PI, G_PI + G_PI_2);
    cairo_close_path(cr);
}

static void paint_text(View *v, cairo_t *cr, const cui_draw_command *draw)
{
    layout_text(v, draw->text, draw->font_size, draw->text_style);
    PangoAttrList *attrs = NULL;
    if (draw->text_color_count) {
        attrs = pango_attr_list_new();
        for (size_t i = 0; i < draw->text_color_count; i++) {
            const cui_text_color_span *span = &draw->text_colors[i];
            PangoAttribute *attr = pango_attr_foreground_new(span->color.r * 257,
                                                            span->color.g * 257, span->color.b * 257);
            size_t offset = draw->text_source.paragraph.data ? draw->text_source.byte_start : 0;
            attr->start_index = (guint)(MAX(span->byte_start, offset) - offset);
            attr->end_index = (guint)(MAX(span->byte_end, offset) - offset);
            pango_attr_list_insert(attrs, attr);
        }
        pango_layout_set_attributes(v->layout, attrs);
        pango_attr_list_unref(attrs);
    }
    int height;
    pango_layout_get_size(v->layout, NULL, &height);
    double y = draw->bounds.y + (draw->bounds.h - (double)height / PANGO_SCALE) / 2;
    cairo_move_to(cr, round(draw->bounds.x), round(y));
    color(cr, draw->color);
    pango_cairo_show_layout(cr, v->layout);
}

static void paint_mask(cairo_t *cr, const cui_draw_command *draw)
{
    for (size_t i = 0; i < ICON_COUNT; i++) {
        if (draw->bitmap.bitmap.pixels != pixels[i])
            continue;
        int size = icons[i].size;
        int stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, size);
        cairo_surface_t *surface = cairo_image_surface_create_for_data(
            (unsigned char *)icons[i].mask, CAIRO_FORMAT_A8, size, size, stride);
        if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
            color(cr, draw->bitmap.tint);
            cairo_mask_surface(cr, surface, round(draw->bounds.x), round(draw->bounds.y));
        }
        cairo_surface_destroy(surface);
        break;
    }
}

static void render(View *v, cairo_surface_t *surface, cui_draw_list list,
                   const System *system, SystemTab tab)
{
    cairo_t *cr = cairo_create(surface);
    color(cr, v->colors[BG]);
    cairo_paint(cr);
    for (size_t i = 0; i < list.count; i++) {
        const cui_draw_command *cmd = &list.commands[i];
        cui_rect r = cmd->bounds;
        switch (cmd->type) {
        case CUI_DRAW_RECT:
            round_rect(cr, r, cmd->radius);
            color(cr, cmd->color);
            cairo_fill(cr);
            break;
        case CUI_DRAW_BORDER: {
            double w = cmd->border_edges.top;
            r.x += w / 2;
            r.y += w / 2;
            r.w -= w;
            r.h -= w;
            round_rect(cr, r, cmd->radius - w / 2);
            color(cr, cmd->color);
            cairo_set_line_width(cr, w);
            cairo_stroke(cr);
            break;
        }
        case CUI_DRAW_TEXT: paint_text(v, cr, cmd); break;
        case CUI_DRAW_IMAGE: paint_mask(cr, cmd); break;
        case CUI_DRAW_CLIP_PUSH:
            cairo_save(cr);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_clip(cr);
            break;
        case CUI_DRAW_CLIP_POP: cairo_restore(cr); break;
        default: break;
        }
    }
    if (system)
        dashboard_plot(v, system, tab, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
}

void view_draw(View *v, const Model *m, const Settings *s, const System *system,
               SystemTab tab, const Calendar *calendar, cairo_surface_t *surface)
{
    cui_context *ui = v->ui;
    if (!cui_begin_frame(ui, (float)v->panel_width, (float)v->panel_height,
                         (double)g_get_monotonic_time() / G_USEC_PER_SEC))
        return;
    v->hit_count = 0;
    v->chart = (cui_rect){0};
    v->today_button = (cui_rect){0};
    memset(v->tabs, 0, sizeof v->tabs);
    cui_begin_box(ui, cui_id_from(ui, "panel"), (cui_box){
        .width = (float)v->panel_width, .height = (float)v->panel_height, .clip = true,
        .padding = (float)v->border_width, .radius = (float)v->radius,
        .background = v->colors[BG], .border = v->colors[BORDER],
        .border_width = (float)v->border_width});
    if (!mode_has_search(m->mode) || model_showcase(m, s)) {
        if (!mode_has_search(m->mode))
            workflow_header(v, m->mode, system);
        else
            header(v, m, s);
        int body = v->height - v->header - v->footer - (v->bar ? 0 : 9);
        if (body > 0) {
            cui_begin_box(ui, 0, (cui_box){.width = -1, .height = body, .clip = true,
                .custom_padding = true, .padding_edges = {v->inset, v->inset, 0, 0}});
            if (m->mode == SYSTEM)
                dashboard_system(v, system, tab, body);
            else if (m->mode == CALENDAR)
                calendar_draw(v, calendar, body);
            else
                dashboard_clock(v, body);
            cui_end_box(ui);
        }
        footer(v, m, s);
    } else if (v->horizontal) {
        horizontal(v, m, s);
    } else {
        header(v, m, s);
        results(v, m);
        footer(v, m, s);
    }
    cui_end_box(ui);
    render(v, surface, cui_end_frame(ui), m->mode == SYSTEM ? system : NULL, tab);
}

int view_tab(const View *v, int x, int y)
{
    for (int i = 0; i < VIEW_TABS; i++) {
        if (view_contains(v->tabs[i], x, y))
            return i;
    }
    return -1;
}

size_t view_hit(const View *v, int x, int y)
{
    for (size_t i = 0; i < v->hit_count; i++) {
        if (view_contains(v->hits[i].bounds, x, y))
            return v->hits[i].rank;
    }
    return SIZE_MAX;
}

bool view_contains(cui_rect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}
