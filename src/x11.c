#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>
#include <cairo-xlib.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include "view.h"
#include "cui_edit.h"

typedef struct { int x, y, width, height; } ScreenRect;

typedef struct {
    Display *display;
    Window root, window, parent, focus;
    int revert, depth;
    Visual *visual;
    XIM im;
    XIC ic;
    Atom utf8, clipboard, paste;
    Pixmap buffer, animation;
    GC gc;
    cairo_surface_t *surface, *glow_surface;
    ScreenRect screen;
    int width, height;
    bool grabbed, shape, window_destroyed;
    KeyCode toggle_key;
    bool toggle_down, toggle_pending, detectable_repeat;
    gint64 opened_at;
    System system;
    SystemTab tab;
    bool system_active;
    Calendar calendar;
    Mode drawn_mode;
    View view;
} Host;

static bool grab(Host *x, Window window)
{
    for (int i = 0; i < 1000; i++) {
        int status = XGrabKeyboard(x->display, window, False, GrabModeAsync, GrabModeAsync,
                                   CurrentTime);
        if (status == GrabSuccess) {
            x->grabbed = true;
            return true;
        }
        g_usleep(1000);
    }
    fprintf(stderr, "launch: could not grab the X11 keyboard\n");
    return false;
}

static int overlap(ScreenRect a, ScreenRect b)
{
    int width = MAX(0, MIN(a.x + a.width, b.x + b.width) - MAX(a.x, b.x));
    int height = MAX(0, MIN(a.y + a.height, b.y + b.height) - MAX(a.y, b.y));
    return width * height;
}

static ScreenRect monitor(Host *x, const Settings *s, const XWindowAttributes *parent)
{
    ScreenRect result = {0, 0, parent->width, parent->height};
    if (x->parent != x->root || !XineramaIsActive(x->display))
        return result;
    int count = 0;
    XineramaScreenInfo *screens = XineramaQueryScreens(x->display, &count);
    if (!screens)
        return result;
    int best = 0, area = 0;
    ScreenRect focus = {0};
    if (x->focus > PointerRoot && x->focus != x->root) {
        XWindowAttributes attr;
        Window child;
        if (XGetWindowAttributes(x->display, x->focus, &attr) &&
            XTranslateCoordinates(x->display, x->focus, x->root, 0, 0, &focus.x, &focus.y, &child)) {
            focus.width = attr.width;
            focus.height = attr.height;
        }
    }
    if (s->monitor >= 0 && s->monitor < count) {
        best = s->monitor;
    } else {
        for (int i = 0; i < count; i++) {
            ScreenRect rect = {screens[i].x_org, screens[i].y_org, screens[i].width, screens[i].height};
            int candidate = overlap(rect, focus);
            if (candidate > area) {
                best = i;
                area = candidate;
            }
        }
        if (!area) {
            Window child, root;
            int px, py, wx, wy;
            unsigned mask;
            if (XQueryPointer(x->display, x->root, &root, &child, &px, &py, &wx, &wy, &mask))
                for (int i = 0; i < count; i++)
                    if (overlap((ScreenRect){px, py, 1, 1}, (ScreenRect){
                        screens[i].x_org, screens[i].y_org, screens[i].width, screens[i].height})) {
                        best = i;
                        break;
                    }
        }
    }
    if (count > 0)
        result = (ScreenRect){screens[best].x_org, screens[best].y_org,
                              screens[best].width, screens[best].height};
    XFree(screens);
    return result;
}

static int ignore_raced_windows(Display *display, XErrorEvent *event)
{
    if (event->error_code == BadWindow || event->error_code == BadDrawable ||
        event->error_code == BadMatch)
        return 0;
    char text[128];
    XGetErrorText(display, event->error_code, text, sizeof text);
    fprintf(stderr, "launch: X11: %s\n", text);
    return 0;
}

static void shape(Host *x, int radius)
{
    if (!x->shape)
        return;
    radius = MIN(radius, MIN(x->width, x->height) / 2);
    if (!radius) {
        XShapeCombineMask(x->display, x->window, ShapeBounding, 0, 0, None, ShapeSet);
        return;
    }
    Pixmap mask = XCreatePixmap(x->display, x->window, (unsigned)x->width, (unsigned)x->height, 1);
    GC gc = XCreateGC(x->display, mask, 0, NULL);
    XSetForeground(x->display, gc, 0);
    XFillRectangle(x->display, mask, gc, 0, 0, (unsigned)x->width, (unsigned)x->height);
    XSetForeground(x->display, gc, 1);
    for (int y = 0; y < radius; y++) {
        double dy = radius - y - 0.5;
        int inset = (int)round(radius - sqrt(radius * radius - dy * dy));
        XFillRectangle(x->display, mask, gc, inset, y, (unsigned)(x->width - 2 * inset), 1);
        XFillRectangle(x->display, mask, gc, inset, x->height - y - 1,
                       (unsigned)(x->width - 2 * inset), 1);
    }
    XFillRectangle(x->display, mask, gc, 0, radius, (unsigned)x->width,
                   (unsigned)(x->height - 2 * radius));
    XShapeCombineMask(x->display, x->window, ShapeBounding, 0, 0, mask, ShapeSet);
    XFreeGC(x->display, gc);
    XFreePixmap(x->display, mask);
}

static void buffer_clear(Host *x)
{
    if (x->glow_surface)
        cairo_surface_destroy(x->glow_surface);
    if (x->animation)
        XFreePixmap(x->display, x->animation);
    if (x->surface)
        cairo_surface_destroy(x->surface);
    if (x->buffer)
        XFreePixmap(x->display, x->buffer);
    x->surface = NULL;
    x->buffer = None;
    x->glow_surface = NULL;
    x->animation = None;
}

static bool resize(Host *x, const Settings *s, bool glow)
{
    if (x->width == x->view.panel_width && x->height == x->view.panel_height &&
        glow == (x->glow_surface != NULL))
        return true;
    buffer_clear(x);
    x->width = x->view.panel_width;
    x->height = x->view.panel_height;
    int dx = (x->screen.width - x->width) / 2;
    int dy = s->placement == BOTTOM ? x->screen.height - x->height :
             s->placement == CENTER ? MIN(x->screen.height / 4, x->screen.height - x->height) : 0;

    dx = CLAMP(dx + s->offset_x, 0, x->screen.width - x->width);
    dy = CLAMP(dy + s->offset_y, 0, x->screen.height - x->height);
    XMoveResizeWindow(x->display, x->window, x->screen.x + dx, x->screen.y + dy,
                      (unsigned)x->width, (unsigned)x->height);
    shape(x, x->view.radius);
    x->buffer = XCreatePixmap(x->display, x->window, (unsigned)x->width, (unsigned)x->height,
                              (unsigned)x->depth);
    x->surface = cairo_xlib_surface_create(x->display, x->buffer, x->visual, x->width, x->height);
    if (glow) {
        x->animation = XCreatePixmap(x->display, x->window, (unsigned)x->width, (unsigned)x->height,
                                     (unsigned)x->depth);
        x->glow_surface = cairo_xlib_surface_create(x->display, x->animation, x->visual, x->width, x->height);
    }
    return cairo_surface_status(x->surface) == CAIRO_STATUS_SUCCESS &&
           (!x->glow_surface || cairo_surface_status(x->glow_surface) == CAIRO_STATUS_SUCCESS);
}

static void present(Host *x)
{
    if (x->buffer)
        XCopyArea(x->display, x->animation ? x->animation : x->buffer, x->window, x->gc, 0, 0, (unsigned)x->width,
                   (unsigned)x->height, 0, 0);
    XFlush(x->display);
}

static void animate(Host *x, const Settings *s, gint64 now)
{
    if (x->glow_surface) {
        double distance = (double)(now - x->opened_at) * s->glow_speed / G_USEC_PER_SEC;
        glow_draw(&x->view, x->glow_surface, x->surface, distance);
    }
    present(x);
}

static bool redraw(Host *x, Model *m, const Settings *s)
{
    gint64 now = g_get_monotonic_time();
    if (m->mode == SYSTEM && !x->system_active) {
        system_open(&x->system, s->system_interval_ms);
        system_sample(&x->system, now);
        x->system_active = true;
        x->tab = CPU;
    } else if (m->mode != SYSTEM && x->system_active) {
        system_close(&x->system);
        x->system_active = false;
    }
    if (m->mode == CALENDAR && x->drawn_mode != CALENDAR)
        calendar_open(&x->calendar);
    x->drawn_mode = m->mode;
    view_fit(&x->view, m, s, &x->calendar, x->screen.width, x->screen.height);
    bool glow = s->border_glow && (!s->glow_search_only || mode_has_search(m->mode));
    if (!resize(x, s, glow))
        return false;
    view_draw(&x->view, m, s, &x->system, x->tab, &x->calendar, x->surface);
    if (cui_last_error(x->view.ui) != CUI_OK) {
        cui_diagnostic diag = {0};
        cui_latest_diagnostic(x->view.ui, &diag);
        fprintf(stderr, "launch: CUI rendering error in %s (%s)\n", diag.operation, diag.message);
        return false;
    }
    animate(x, s, now);
    return true;
}

static bool host_setup(Host *x, Model *m, const Settings *s)
{
    XWindowAttributes parent;
    if (!XGetWindowAttributes(x->display, x->parent, &parent)) {
        fprintf(stderr, "launch: invalid embedding window 0x%lx\n", x->parent);
        return false;
    }
    XGetInputFocus(x->display, &x->focus, &x->revert);
    x->screen = monitor(x, s, &parent);
    x->depth = parent.depth;
    x->visual = parent.visual;
    if (!view_init(&x->view, s, x->display, parent.colormap))
        return false;

    XSetWindowAttributes attr = {
        .override_redirect = True, .background_pixel = 0,
        .event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | VisibilityChangeMask | StructureNotifyMask |
                      ButtonPressMask | ButtonReleaseMask | FocusChangeMask,
    };
    x->window = XCreateWindow(x->display, x->parent, 0, 0, 1, 1, 0, CopyFromParent,
                               InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel |
                               CWEventMask, &attr);
    XGCValues gc_values = {.graphics_exposures = False};
    x->gc = XCreateGC(x->display, x->window, GCGraphicsExposures, &gc_values);
    XClassHint hint = {s->compatible ? "dmenu" : "launch", s->compatible ? "dmenu" : "Launch"};
    XSetClassHint(x->display, x->window, &hint);
    XStoreName(x->display, x->window, s->compatible ? "dmenu" : "Launch");
    x->clipboard = XInternAtom(x->display, "CLIPBOARD", False);
    x->utf8 = XInternAtom(x->display, "UTF8_STRING", False);
    x->paste = XInternAtom(x->display, "_LAUNCH_PASTE", False);
    int event, error;
    x->shape = XShapeQueryExtension(x->display, &event, &error);

    XSetLocaleModifiers("");
    x->im = XOpenIM(x->display, NULL, NULL, NULL);
    if (x->im) {
        x->ic = XCreateIC(x->im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                          XNClientWindow, x->window, XNFocusWindow, x->window, NULL);
        if (x->ic) {
            long mask = 0;
            XGetICValues(x->ic, XNFilterEvents, &mask, NULL);
            XSelectInput(x->display, x->window, attr.event_mask | mask);
            XSetICFocus(x->ic);
        }
    }
    model_filter(m, s);
    x->opened_at = g_get_monotonic_time();
    if (!redraw(x, m, s))
        return false;
    XMapRaised(x->display, x->window);
    present(x);
    if (x->parent != x->root) {
        XSetInputFocus(x->display, x->window, RevertToParent, CurrentTime);
        return true;
    }
    return grab(x, x->window);
}

static void prepare_toggle(Host *x, const Settings *s)
{
    if (s->compatible)
        return;
    Bool detectable = False;
    XkbSetDetectableAutoRepeat(x->display, True, &detectable);
    x->detectable_repeat = detectable;
    x->toggle_key = XKeysymToKeycode(x->display, s->shortcut[KEY_TOGGLE].symbol);
    if (x->toggle_key) {
        char held[32];
        XQueryKeymap(x->display, held);
        x->toggle_down = ((unsigned char)held[x->toggle_key / 8] & (1u << (x->toggle_key % 8))) != 0;
    }
}

static bool toggle_event(Host *x, Model *m, const Settings *s, const XEvent *event)
{
    if ((event->type != KeyPress && event->type != KeyRelease) ||
        !x->toggle_key || event->xkey.keycode != x->toggle_key)
        return false;
    if (event->type == KeyPress) {
        unsigned modifiers = event->xkey.state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
        bool chord = modifiers == s->shortcut[KEY_TOGGLE].modifiers;
        if (chord && !x->toggle_down)
            x->toggle_pending = s->toggle;
        x->toggle_down = true;
        return chord || x->toggle_pending;
    }
    if (!x->detectable_repeat && XEventsQueued(x->display, QueuedAfterReading)) {
        XEvent next;
        XPeekEvent(x->display, &next);
        if (next.type == KeyPress && next.xkey.keycode == x->toggle_key &&
            next.xkey.time == event->xkey.time)
            return true;
    }
    x->toggle_down = false;
    if (x->toggle_pending)
        m->done = true;
    x->toggle_pending = false;
    return true;
}

static void host_clear(Host *x)
{
    if (x->system_active)
        system_close(&x->system);
    if (!x->display)
        return;
    if (x->grabbed)
        XUngrabKeyboard(x->display, CurrentTime);
    if (x->ic)
        XDestroyIC(x->ic);
    if (x->im)
        XCloseIM(x->im);
    if (x->parent != x->root && x->window) {
        Window current;
        int revert;
        XGetInputFocus(x->display, &current, &revert);
        if (current == x->window && x->focus != x->window)
            XSetInputFocus(x->display, x->focus, x->revert, CurrentTime);
    }
    buffer_clear(x);
    if (x->gc)
        XFreeGC(x->display, x->gc);
    if (x->window && !x->window_destroyed)
        XDestroyWindow(x->display, x->window);
    view_clear(&x->view);
    XCloseDisplay(x->display);
}

static size_t rune(const Model *m, size_t from, int direction)
{
    if (direction < 0)
        return from ? (size_t)(g_utf8_prev_char(m->query + from) - m->query) : 0;
    return m->query[from] ? (size_t)(g_utf8_next_char(m->query + from) - m->query) : from;
}

static size_t grapheme(const Model *m, const Settings *s, int direction)
{
    if (s->compatible)
        return rune(m, m->cursor, direction);
    cui_str text = cui_string(m->query);
    return direction < 0 ? cui_grapheme_previous(text, m->cursor)
                         : cui_grapheme_next(text, m->cursor);
}

static bool whitespace(const char *text, size_t start, size_t end)
{
    for (const char *p = text + start; p < text + end; p = g_utf8_next_char(p))
        if (!g_unichar_isspace(g_utf8_get_char(p)))
            return false;
    return true;
}

static size_t unicode_word(const Model *m, int direction)
{
    cui_str text = cui_string(m->query);
    size_t start = m->cursor, end;
    do {
        end = direction < 0 ? cui_word_previous(text, start) : cui_word_next(text, start);
        if (end == SIZE_MAX)
            return m->cursor;
        if (start == end || !whitespace(text.data, MIN(start, end), MAX(start, end)))
            return end;
        start = end;
    } while (true);
}

static size_t word(const Model *m, const Settings *s, int direction)
{
    if (!s->compatible)
        return unicode_word(m, direction);
    size_t i = m->cursor;
    if (direction < 0) {
        while (i && m->query[rune(m, i, -1)] == ' ')
            i = rune(m, i, -1);
        while (i && m->query[rune(m, i, -1)] != ' ')
            i = rune(m, i, -1);
    } else {
        while (m->query[i] == ' ')
            i = rune(m, i, 1);
        while (m->query[i] && m->query[i] != ' ')
            i = rune(m, i, 1);
    }
    return i;
}

static void move(Model *m, const View *v, long offset)
{
    long index = (long)m->selected + offset;
    if (index < 0 || (size_t)index >= m->matches->len)
        return;
    size_t target = (size_t)index;
    if (v->horizontal && target >= m->first + m->page)
        m->first = target;
    model_select(m, target);
}

static void change_page(Model *m, View *v, const Settings *s, bool advance)
{
    size_t target = advance ? m->first + m->page : view_previous_page(v, m, s);
    if (!m->page || target >= m->matches->len || target == m->first)
        return;
    m->first = target;
    model_select(m, target);
}

static void request_paste(Host *x, Atom selection)
{
    XConvertSelection(x->display, selection, x->utf8, x->paste, x->window, CurrentTime);
}

static void receive_paste(Host *x, Model *m, const XSelectionEvent *event)
{
    if (!event->property) {
        if (event->target == x->utf8)
            XConvertSelection(x->display, event->selection, XA_STRING, x->paste, x->window, CurrentTime);
        return;
    }
    Atom format;
    int bits;
    unsigned long size, after;
    unsigned char *text = NULL;
    if (XGetWindowProperty(x->display, x->window, x->paste, 0, QUERY_CAPACITY / 4,
                           True, AnyPropertyType, &format, &bits, &size, &after,
                           &text) == Success && bits == 8 && text && !after) {
        size_t length = strcspn((const char *)text, "\n\r");
        if (format == x->utf8)
            model_insert(m, (const char *)text, length);
        else if (format == XA_STRING) {
            gsize written;
            g_autofree char *utf8 = g_convert((const char *)text, (gssize)length, "UTF-8",
                                               "ISO-8859-1", NULL, &written, NULL);
            if (utf8)
                model_insert(m, utf8, written);
        }
    }
    if (text)
        XFree(text);
}

static void field_arrow(Model *m, const Settings *s, int direction, bool shift)
{
    size_t from, to;
    size_t target = grapheme(m, s, direction);
    if (!shift && model_selection(m, &from, &to))
        target = direction < 0 ? from : to;
    model_seek(m, target, shift && !s->compatible);
}

static void select_tab(Host *x, Mode mode, int tab)
{
    if (mode == SYSTEM)
        x->tab = (SystemTab)tab;
    else
        x->calendar.tab = (CalendarTab)tab;
}

static void workflow_key(Host *x, Model *m, const Settings *s, KeySym symbol, unsigned state)
{
    if ((state & ControlMask) && (symbol == XK_c || symbol == XK_g || symbol == XK_bracketleft))
        symbol = XK_Escape;
    if (symbol == XK_Escape) {
        if (!model_back(m, s))
            m->done = true;
        return;
    }
    if (state & (ControlMask | Mod1Mask | Mod4Mask))
        return;

    bool up = symbol == XK_Up || symbol == XK_KP_Up;
    if (up || symbol == XK_Down || symbol == XK_KP_Down) {
        if (m->mode == SYSTEM)
            system_step_interval(&x->system, up);
        else
            calendar_shift(&x->calendar, up ? -1 : 1);
    } else if (m->mode == CALENDAR && (symbol == XK_t || symbol == XK_T)) {
        calendar_go_today(&x->calendar);
    } else {
        bool left = symbol == XK_Left || symbol == XK_KP_Left || symbol == XK_ISO_Left_Tab ||
                    (symbol == XK_Tab && (state & ShiftMask));
        if (left || symbol == XK_Right || symbol == XK_KP_Right || symbol == XK_Tab) {
            int count = m->mode == SYSTEM ? SYSTEM_TABS : CALENDAR_TABS;
            int current = m->mode == SYSTEM ? (int)x->tab : (int)x->calendar.tab;
            select_tab(x, m->mode, (current + (left ? -1 : 1) + count) % count);
        } else if (symbol >= XK_1 && symbol <= XK_5) {
            select_tab(x, m->mode, symbol - XK_1);
        }
    }
}

static bool key(Host *x, Model *m, const Settings *s, XKeyEvent *event)
{
    char bytes[QUERY_CAPACITY];
    KeySym symbol = NoSymbol;
    int count;
    if (x->ic) {
        Status status;
        count = Xutf8LookupString(x->ic, event, bytes, sizeof bytes, &symbol, &status);
        if (status == XBufferOverflow || status == XLookupNone)
            return true;
        if (status == XLookupChars) {
            if (mode_has_search(m->mode))
                model_insert(m, bytes, (size_t)count);
            return true;
        }
    } else {
        count = XLookupString(event, bytes, sizeof bytes, &symbol, NULL);
    }

    unsigned state = event->state;
    static const KeySym aliases[] = {XK_Up, XK_Down, XK_Return, XK_Escape};
    unsigned relevant = state & (ControlMask | Mod1Mask | ShiftMask | Mod4Mask);
    KeySym lower, upper;
    XConvertCase(symbol, &lower, &upper);
    for (size_t i = 0; i < G_N_ELEMENTS(aliases); i++) {
        const Shortcut *shortcut = &s->shortcut[i];
        if (shortcut->symbol && relevant == shortcut->modifiers &&
            (symbol == shortcut->symbol || ((relevant & ShiftMask) && lower == shortcut->symbol))) {
            symbol = aliases[i];
            state = 0;
            break;
        }
    }
    if (!mode_has_search(m->mode)) {
        workflow_key(x, m, s, symbol, state);
        return true;
    }
    bool ctrl = state & ControlMask;
    bool shift = state & ShiftMask;
    if (ctrl) {
        if (!s->compatible)
            symbol = lower;
        switch (symbol) {
        case XK_a:
            if (!s->compatible) {
                model_seek(m, 0, false);
                model_seek(m, strlen(m->query), true);
                return true;
            }
            symbol = XK_Home;
            break;
        case XK_b:
            if (m->mode == EMOJI) {
                model_seek(m, grapheme(m, s, -1), shift && !s->compatible);
                return true;
            }
            symbol = XK_Left;
            break;
        case XK_c: case XK_g: case XK_bracketleft: symbol = XK_Escape; break;
        case XK_d: symbol = XK_Delete; break;
        case XK_BackSpace:
            if (!s->compatible)
                model_erase(m, word(m, s, -1), m->cursor);
            return true;
        case XK_Delete: case XK_KP_Delete:
            if (!s->compatible)
                model_erase(m, m->cursor, word(m, s, 1));
            return true;
        case XK_e:
            if (!s->compatible)
                return true;
            symbol = XK_End;
            break;
        case XK_f:
            if (m->mode == EMOJI) {
                model_seek(m, grapheme(m, s, 1), shift && !s->compatible);
                return true;
            }
            symbol = XK_Right;
            break;
        case XK_h: symbol = XK_BackSpace; break;
        case XK_i: symbol = XK_Tab; break;
        case XK_j: case XK_J: case XK_m: case XK_M: symbol = XK_Return; ctrl = false; break;
        case XK_n: symbol = XK_Down; break;
        case XK_p: symbol = XK_Up; break;
        case XK_k: model_erase(m, m->cursor, strlen(m->query)); return true;
        case XK_u: model_erase(m, 0, m->cursor); return true;
        case XK_w: model_erase(m, word(m, s, -1), m->cursor); return true;
        case XK_y: case XK_Y: request_paste(x, shift ? x->clipboard : XA_PRIMARY); return true;
        case XK_v: case XK_V:
            if (!s->compatible)
                request_paste(x, x->clipboard);
            return true;
        case XK_Left: case XK_KP_Left: model_seek(m, word(m, s, -1), shift && !s->compatible); return true;
        case XK_Right: case XK_KP_Right: model_seek(m, word(m, s, 1), shift && !s->compatible); return true;
        case XK_Return: case XK_KP_Enter: break;
        default: return true;
        }
    } else if (state & Mod1Mask) {
        switch (symbol) {
        case XK_Return: case XK_KP_Enter:
            return s->compatible ? true : model_reveal(m, s);
        case XK_b: model_seek(m, word(m, s, -1), shift && !s->compatible); return true;
        case XK_f: model_seek(m, word(m, s, 1), shift && !s->compatible); return true;
        case XK_g: symbol = XK_Home; break;
        case XK_G: symbol = XK_End; break;
        case XK_h: symbol = XK_Up; break;
        case XK_j: symbol = XK_Next; break;
        case XK_k: symbol = XK_Prior; break;
        case XK_l: symbol = XK_Down; break;
        default: return true;
        }
    }

    switch (symbol) {
    case XK_Escape:
        if (!model_back(m, s))
            m->done = true;
        break;
    case XK_Return: case XK_KP_Enter:
        return model_accept(m, s, x->display, shift, ctrl);
    case XK_Home: case XK_KP_Home:
        if (!m->selected || !s->compatible)
            model_seek(m, 0, shift && !s->compatible);
        else
            model_select(m, 0);
        break;
    case XK_End: case XK_KP_End:
        if (m->query[m->cursor] || !s->compatible)
            model_seek(m, strlen(m->query), shift && !s->compatible);
        else if (m->matches->len)
            model_select(m, m->matches->len - 1);
        break;
    case XK_BackSpace:
        model_erase(m, grapheme(m, s, -1), m->cursor);
        break;
    case XK_Delete: case XK_KP_Delete:
        model_erase(m, m->cursor, grapheme(m, s, 1));
        break;
    case XK_Left: case XK_KP_Left:
        if (m->mode == EMOJI || (x->view.horizontal && m->selected > 0))
            move(m, &x->view, -1);
        else
            field_arrow(m, s, -1, shift);
        break;
    case XK_Right: case XK_KP_Right:
        if (m->mode == EMOJI || (x->view.horizontal && !m->query[m->cursor]))
            move(m, &x->view, 1);
        else
            field_arrow(m, s, 1, shift);
        break;
    case XK_Up: case XK_KP_Up: move(m, &x->view, -(long)m->columns); break;
    case XK_Down: case XK_KP_Down: move(m, &x->view, (long)m->columns); break;
    case XK_Prior: case XK_KP_Prior: change_page(m, &x->view, s, false); break;
    case XK_Next: case XK_KP_Next: change_page(m, &x->view, s, true); break;
    case XK_Tab: case XK_ISO_Left_Tab: {
        if (m->mode != PICK)
            break;
        Item *item = model_item(m, m->selected);
        if (!item)
            break;
        m->query[0] = '\0';
        model_seek(m, 0, false);
        size_t length = MIN(strlen(item->label), sizeof m->query - 1);
        while (length && (((unsigned char)item->label[length]) & 0xc0) == 0x80)
            length--;
        model_insert(m, item->label, length);
        m->dirty = true;
        break;
    }
    default:
        if (count > 0 && !g_ascii_iscntrl(bytes[0]))
            model_insert(m, bytes, (size_t)count);
        break;
    }
    return true;
}

static bool loop(Host *x, Model *m, const Settings *s)
{
    const gint64 blink = 500 * G_TIME_SPAN_MILLISECOND;
    const gint64 frame = G_TIME_SPAN_SECOND / 30;
    gint64 caret_at = g_get_monotonic_time() + blink, glow_at = x->opened_at + frame;
    gint64 clock_at = 0, date_at = 0;
    struct pollfd connections[] = {{.fd = ConnectionNumber(x->display), .events = POLLIN},
                                   {.fd = -1, .events = POLLIN},
                                   {.fd = -1, .events = POLLIN}};
    while (!m->done) {
        if (!s->compatible) {
            gint64 now = g_get_monotonic_time();
            gint64 deadline = G_MAXINT64;
            bool refresh = model_file_update(m);
            refresh |= model_rates_update(m, s);
            if (mode_has_search(m->mode) && now >= caret_at) {
                x->view.caret_visible = !x->view.caret_visible;
                caret_at = now + blink;
                refresh = true;
            }
            if (mode_has_search(m->mode))
                deadline = caret_at;
            if (model_showcase(m, s) && s->startup == CLOCK) {
                if (now >= clock_at) {
                    clock_at = now + G_TIME_SPAN_SECOND - g_get_real_time() % G_TIME_SPAN_SECOND;
                    refresh = true;
                }
                deadline = MIN(deadline, clock_at);
            } else {
                clock_at = 0;
            }
            if (m->mode == CALENDAR) {
                if (now >= date_at) {
                    const gint64 minute = G_TIME_SPAN_MINUTE;
                    date_at = now + minute - g_get_real_time() % minute;
                    refresh |= calendar_update_today(&x->calendar);
                }
                deadline = MIN(deadline, date_at);
            } else {
                date_at = 0;
            }
            if (x->system_active) {
                gint64 interval = x->system.interval_ms * G_TIME_SPAN_MILLISECOND;
                if (now - x->system.last_sample >= interval) {
                    system_sample(&x->system, now);
                    refresh = true;
                }
                deadline = MIN(deadline, x->system.last_sample + interval);
            }
            if (refresh && !redraw(x, m, s))
                return false;
            if (x->glow_surface) {
                if (now >= glow_at) {
                    if (!refresh)
                        animate(x, s, now);
                    glow_at = now + frame;
                }
                deadline = MIN(deadline, glow_at);
            }
            if (!XPending(x->display)) {
                int timeout = (int)MAX(0, (deadline - g_get_monotonic_time() + 999) / 1000);
                connections[1].fd = files_fd(m->files);
                connections[2].fd = rates_fd(m->rates);
                if (poll(connections, G_N_ELEMENTS(connections), timeout) < 0 && errno != EINTR)
                    return false;
                continue;
            }
        }
        XEvent event;
        XNextEvent(x->display, &event);
        if (toggle_event(x, m, s, &event) || XFilterEvent(&event, x->window))
            continue;
        switch (event.type) {
        case KeyPress:
            if (!key(x, m, s, &event.xkey))
                return false;
            break;
        case ButtonPress:
            if (s->compatible)
                continue;
            if (!mode_has_search(m->mode)) {
                if (event.xbutton.button == Button1) {
                    int px = event.xbutton.x, py = event.xbutton.y;
                    int tab = view_tab(&x->view, px, py);
                    if (tab >= 0)
                        select_tab(x, m->mode, tab);
                    else if (m->mode == CALENDAR && view_contains(x->view.today_button, px, py))
                        calendar_go_today(&x->calendar);
                }
            } else if (event.xbutton.button == Button4 || event.xbutton.button == Button5)
                move(m, &x->view, event.xbutton.button == Button4 ? -(long)m->columns : (long)m->columns);
            else if (event.xbutton.button == Button2)
                request_paste(x, XA_PRIMARY);
            else if (event.xbutton.button == Button1) {
                size_t rank = view_hit(&x->view, event.xbutton.x, event.xbutton.y);
                if (rank != SIZE_MAX) {
                    model_select(m, rank);
                    if (!model_accept(m, s, x->display, false, false))
                        return false;
                }
            }
            break;
        case SelectionNotify:
            if (mode_has_search(m->mode))
                receive_paste(x, m, &event.xselection);
            break;
        case DestroyNotify:
            x->window_destroyed = true;
            return true;
        case FocusOut:
            if (s->parent && event.xfocus.mode == NotifyNormal)
                XSetInputFocus(x->display, x->window, RevertToParent, CurrentTime);
            continue;
        case VisibilityNotify:
            if (event.xvisibility.state != VisibilityUnobscured)
                XRaiseWindow(x->display, x->window);
            continue;
        case Expose:
            if (event.xexpose.count == 0)
                present(x);
            continue;
        default: continue;
        }
        if (!m->done) {
            x->view.caret_visible = true;
            caret_at = g_get_monotonic_time() + blink;
            model_filter(m, s);
            if (!redraw(x, m, s))
                return false;
        }
    }
    return true;
}

int x11_run(Settings *s, Mode mode, int argc, char **argv)
{
    Host x = {0};
    Model model;
    model_init(&model, mode);
    bool ok = false;
    x.display = XOpenDisplay(NULL);
    if (!x.display) {
        fprintf(stderr, "launch: could not open the X11 display\n");
        goto finish;
    }
    XSetErrorHandler(ignore_raced_windows);
    x.root = DefaultRootWindow(x.display);
    settings_resources(s, x.display);
    if (!settings_config(s) || !settings_args(s, argc, argv, &mode))
        goto finish;
    x.parent = s->parent ? s->parent : x.root;
    if (mode == PICK && s->fast && !isatty(STDIN_FILENO) && !s->parent && !grab(&x, x.root))
        goto finish;
    if (!model_load(&model, s, x.display)) {
        fprintf(stderr, "launch: %s\n", model.error);
        goto finish;
    }
    if (!host_setup(&x, &model, s))
        goto finish;
    prepare_toggle(&x, s);
    ok = loop(&x, &model, s) && model.accepted;

finish:
    host_clear(&x);
    if (ok && model.output && !clipboard_copy(model.output)) {
        fprintf(stderr, "launch: could not claim the X11 clipboard\n");
        ok = false;
    }
    if (ok)
        ok = model_dispatch(&model, s);
    model_clear(&model);
    return ok ? 0 : 1;
}
