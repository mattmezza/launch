#include <math.h>
#include "view.h"

typedef struct { double x, y; } Point;

static Point perimeter_point(double w, double h, double r, double distance)
{
    const double quarter = r * G_PI_2;
    const double length[] = {w - 2*r, quarter, h - 2*r, quarter,
                             w - 2*r, quarter, h - 2*r, quarter};
    const Point starts[] = {{r, 0}, {w - r, r}, {w, r}, {w - r, h - r},
                            {w - r, h}, {r, h - r}, {0, h - r}, {r, r}};
    const Point directions[] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    distance = fmod(distance, 2 * (w + h - 4*r) + 4 * quarter);
    if (distance < 0)
        distance += 2 * (w + h - 4*r) + 4 * quarter;
    for (int i = 0; i < 8; i++) {
        if (distance < length[i]) {
            Point p = starts[i];
            if (i % 2) {
                double angle = (i / 2 - 1) * G_PI_2 + distance / r;
                return (Point){p.x + r * cos(angle), p.y + r * sin(angle)};
            }
            Point direction = directions[i / 2];
            return (Point){p.x + distance * direction.x, p.y + distance * direction.y};
        }
        distance -= length[i];
    }
    return starts[0];
}

static void outline(cairo_t *cr, double w, double h, double r, double inset)
{
    double right = w - inset, bottom = h - inset;
    if (right <= inset || bottom <= inset)
        return;
    if (r <= inset) {
        cairo_rectangle(cr, inset, inset, right - inset, bottom - inset);
        return;
    }
    double radius = r - inset;
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r, r, radius, -G_PI_2, 0);
    cairo_arc(cr, w - r, h - r, radius, 0, G_PI_2);
    cairo_arc(cr, r, h - r, radius, G_PI_2, G_PI);
    cairo_arc(cr, r, r, radius, G_PI, G_PI + G_PI_2);
    cairo_close_path(cr);
}

static void spot(cairo_t *cr, Point p, double radius, cui_color tint, double intensity)
{
    cairo_pattern_t *glow = cairo_pattern_create_radial(p.x, p.y, 0, p.x, p.y, radius);
    double r = tint.r / 255.0, g = tint.g / 255.0, b = tint.b / 255.0;
    cairo_pattern_add_color_stop_rgba(glow, 0, r, g, b, intensity);
    cairo_pattern_add_color_stop_rgba(glow, 0.33, r, g, b, intensity * 0.62);
    cairo_pattern_add_color_stop_rgba(glow, 0.64, r, g, b, intensity * 0.18);
    cairo_pattern_add_color_stop_rgba(glow, 1, r, g, b, 0);
    cairo_set_source(cr, glow);
    cairo_paint(cr);
    cairo_pattern_destroy(glow);
}

void glow_draw(const View *v, cairo_surface_t *dest, cairo_surface_t *base, double distance)
{
    cairo_t *cr = cairo_create(dest);
    cairo_set_source_surface(cr, base, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    double w = v->panel_width, h = v->panel_height, r = v->radius;
    double tail = MIN(42.0, (w + h) / 12);
    Point head = perimeter_point(w, h, r, distance);
    Point behind = perimeter_point(w, h, r, distance - tail);
    Point end = perimeter_point(w, h, r, distance - 2 * tail);
    cui_color tint = v->colors[ACCENT];

    outline(cr, w, h, r, 0);
    cairo_clip(cr);
    double spread = CLAMP(h * 0.73, 92.0, 220.0);
    spot(cr, head, spread, tint, 0.115);
    spot(cr, behind, spread * 0.75, tint, 0.060);
    spot(cr, head, 34, tint, 0.095);

    if (v->border_width) {
        outline(cr, w, h, r, 0);
        outline(cr, w, h, r, v->border_width);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_clip(cr);
        spot(cr, end, 49, tint, 0.22);
        spot(cr, behind, 61, tint, 0.43);
        spot(cr, head, 53, tint, 0.92);
    }
    cairo_destroy(cr);
    cairo_surface_flush(dest);
}
