#include <X11/Xatom.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "launch.h"

static void serve(Display *display, const XSelectionRequestEvent *request,
                  const char *text, Atom targets, Atom utf8, Atom plain)
{
    XEvent reply = {.xselection = {
        .type = SelectionNotify, .display = display, .requestor = request->requestor,
        .selection = request->selection, .target = request->target, .time = request->time,
    }};
    Atom property = request->property ? request->property : request->target;
    if (request->target == targets) {
        Atom supported[] = {targets, utf8, plain, XA_STRING};
        XChangeProperty(display, request->requestor, property, XA_ATOM, 32, PropModeReplace,
                        (const unsigned char *)supported, G_N_ELEMENTS(supported));
        reply.xselection.property = property;
    } else if (request->target == utf8 || request->target == plain) {
        XChangeProperty(display, request->requestor, property, utf8, 8, PropModeReplace,
                        (const unsigned char *)text, (int)strlen(text));
        reply.xselection.property = property;
    } else if (request->target == XA_STRING) {
        gsize length;
        g_autofree char *latin = g_convert(text, -1, "ISO-8859-1", "UTF-8", NULL, &length, NULL);
        if (latin) {
            XChangeProperty(display, request->requestor, property, XA_STRING, 8, PropModeReplace,
                            (const unsigned char *)latin, (int)length);
            reply.xselection.property = property;
        }
    }
    XSendEvent(display, request->requestor, False, 0, &reply);
    XFlush(display);
}

static int ignore_xerror(Display *display, XErrorEvent *event)
{
    (void)display;
    (void)event;
    return 0;
}

int clipboard_serve(int ready, const char *text)
{
    signal(SIGPIPE, SIG_IGN);
    Display *display = XOpenDisplay(NULL);
    if (!display)
        return 1;
    XSetErrorHandler(ignore_xerror);
    Window owner = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
    Atom targets = XInternAtom(display, "TARGETS", False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    Atom plain = XInternAtom(display, "text/plain;charset=utf-8", False);
    XSetSelectionOwner(display, clipboard, owner, CurrentTime);
    if (XGetSelectionOwner(display, clipboard) != owner) {
        XCloseDisplay(display);
        return 1;
    }
    if (write(ready, "1", 1) != 1) {
        XCloseDisplay(display);
        return 1;
    }
    close(ready);
    for (;;) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type == SelectionClear)
            break;
        if (event.type == SelectionRequest)
            serve(display, &event.xselectionrequest, text, targets, utf8, plain);
    }
    XCloseDisplay(display);
    return 0;
}

bool clipboard_copy(const char *text)
{
    int ready[2];
    if (pipe(ready) < 0)
        return false;
    char arg[16];
    g_snprintf(arg, sizeof arg, "%d", ready[1]);
    pid_t child = fork();
    if (child == 0) {
        close(ready[0]);
        setsid();
        int sink = open("/dev/null", O_RDWR);
        if (sink >= 0) {
            for (int fd = 0; fd <= STDERR_FILENO; fd++)
                if (fd != ready[1])
                    dup2(sink, fd);
            if (sink > STDERR_FILENO)
                close(sink);
        }
        execl("/proc/self/exe", "launch", "--serve-selection", arg, text, (char *)NULL);
        _exit(1);
    }

    close(ready[1]);
    struct pollfd fds = {.fd = ready[0], .events = POLLIN};
    bool ok = false;
    if (child > 0) {
        int result;
        do {
            result = poll(&fds, 1, 2000);
        } while (result < 0 && errno == EINTR);
        char signal_byte;
        ok = result > 0 && read(ready[0], &signal_byte, 1) == 1 && signal_byte == '1';
    }
    close(ready[0]);
    return ok;
}
