"""Exercise the actual launcher through an isolated X11 keyboard and clipboard."""

import contextlib
import ctypes as C
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
LAUNCH = ROOT / "build" / "launch"
DMENU = ROOT / "build" / "dmenu"


def xdotool(*args):
    return subprocess.run(["xdotool", *map(str, args)], capture_output=True, text=True, timeout=3)


def window(name, app):
    end = time.monotonic() + 4
    while time.monotonic() < end:
        if app.poll() is not None:
            raise AssertionError(f"{name} exited early: {app.communicate()}")
        result = xdotool("search", "--onlyvisible", "--name", f"^{re.escape(name)}$")
        if result.returncode == 0:
            time.sleep(0.04)
            return int(result.stdout.splitlines()[-1])
        time.sleep(0.02)
    raise AssertionError(f"no mapped window named {name}")


@contextlib.contextmanager
def application(*argv, lines=None):
    app = subprocess.Popen(argv, stdin=subprocess.PIPE if lines is not None else subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        if lines is not None:
            app.stdin.write(lines)
            app.stdin.close()
            app.stdin = None
        yield app
    finally:
        if app.poll() is None:
            app.terminate()
        app.wait(timeout=3)


def keys(*names):
    xdotool("key", "--clearmodifiers", *names).check_returncode()


def type_text(text):
    xdotool("type", "--clearmodifiers", text).check_returncode()


def check_exit(app, expected, status=0):
    out, err = app.communicate(timeout=4)
    assert (app.returncode, out, err) == (status, expected, ""), (app.returncode, out, err)


def menu(argv, data, action, expected, status=0):
    with application(*argv, lines=data) as app:
        win = window("dmenu" if argv[0] == DMENU else "Launch", app)
        action(win)
        check_exit(app, expected, status)


def wait_file(path, expected):
    end = time.monotonic() + 3
    while time.monotonic() < end:
        if path.exists() and path.read_text() == expected:
            return
        time.sleep(0.02)
    raise AssertionError(f"{path} was not written with {expected!r}")


def clipboard():
    return subprocess.check_output(["xclip", "-selection", "clipboard", "-o"], text=True, timeout=3)


def check_conversions(tmp, bin_dir):
    with application(LAUNCH) as app:
        window("Launch", app)
        type_text("5 mi to km")
        keys("Return")
        check_exit(app, "")
    assert clipboard() == "8.04672 km"

    for query, answer in (("2.5 GiB to MiB", "2560 MiB"), ("1 MB to Mb", "8 Mb"),
                          ("1 US gal to L", "3.785411784 L")):
        menu([LAUNCH, "calc", "--print"], None,
             lambda _: (type_text(query), keys("Return")), answer + "\n")
    menu([LAUNCH, "calc", "--print"], None,
         lambda _: (type_text("1 kg to"), keys("Down", "Return")), "1000 g\n")
    menu([LAUNCH, "calc", "--print"], None,
         lambda _: (type_text("5 mi to km"), keys("ctrl+a"), type_text("5 mi to kg"),
                    keys("Return", "Escape")), "", 1)

    subprocess.run(["xclip", "-selection", "clipboard"], input="72°F to °C", text=True,
                   check=True, timeout=3)
    with application(LAUNCH) as app:
        window("Launch", app)
        keys("ctrl+v")
        time.sleep(0.1)
        keys("Return")
        check_exit(app, "")
    assert clipboard() == "22.22 °C"

    feed = tmp / "ecb.xml"
    xml = ("<?xml version='1.0'?><gesmes:Envelope>\n"
           "<Cube><Cube time='2026-03-17'><Cube currency='USD' rate='1.2'/>"
           "<Cube currency='CHF' rate='0.9'/><Cube currency='JPY' rate='155'/></Cube></Cube>\n"
           "</gesmes:Envelope>\n")
    feed.write_text(xml)
    args = tmp / "rates-arguments"
    ready = tmp / "rates-ready"
    curl = bin_dir / "curl"
    curl.write_text(
        "#!/bin/sh\n"
        f"printf '%s\\n' \"$@\" > '{args}'\n"
        "for n in $(seq 1 100); do\n"
        f"    if test -f '{ready}'; then exec cat '{feed}'; fi\n"
        "    sleep 0.03\n"
        "done\nexit 1\n"
    )
    curl.chmod(0o755)
    request = ("-q\n--fail\n--silent\n--proto\n=https\n--connect-timeout\n3\n--max-time\n8\n"
               "--max-filesize\n65536\nhttps://www.ecb.europa.eu/stats/eurofxref/eurofxref-daily.xml\n")
    cache = tmp / "cache" / "launch" / "ecb-rates.xml"
    with application(LAUNCH) as app:
        win = window("Launch", app)
        type_text("100 usd to chf")
        wait_file(args, request)
        pending = xdotool("getwindowgeometry", win).stdout
        keys("Return")
        assert app.poll() is None
        ready.touch()
        wait_file(cache, xml)
        wait_geometry(win, lambda geometry: geometry != pending)
        keys("Return")
        check_exit(app, "")
    assert clipboard() == "75.00 CHF"

    args.unlink()
    menu([LAUNCH, "calc", "--print"], None,
         lambda _: (type_text("2 EUR to JPY"), keys("Return")), "310 JPY\n")
    assert not args.exists()

    # An expired cache still works while the next request fails in the background.
    lock = cache.with_suffix(".xml.lock")
    os.utime(cache, (1, 1))
    os.utime(lock, (1, 1))
    curl.write_text(f"#!/bin/sh\nprintf '%s\\n' \"$@\" > '{args}'\nexit 1\n")
    menu([LAUNCH, "calc", "--print"], None,
         lambda _: (type_text("100 USD to CHF"), keys("Return")), "75.00 CHF\n")
    wait_file(args, request)
    assert cache.read_text() == xml

    cache.unlink()
    menu([LAUNCH, "calc", "--print"], None,
         lambda _: (type_text("100 USD to CHF"), keys("Return", "Escape")), "", 1)


def wait_geometry(win, predicate):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        geometry = xdotool("getwindowgeometry", win).stdout
        if predicate(geometry):
            return geometry
        time.sleep(0.02)
    raise AssertionError(geometry)


@contextlib.contextmanager
def x_connection():
    lib = C.CDLL("libX11.so.6")
    lib.XOpenDisplay.argtypes = [C.c_char_p]
    lib.XOpenDisplay.restype = C.c_void_p
    lib.XDefaultRootWindow.argtypes = [C.c_void_p]
    lib.XDefaultRootWindow.restype = C.c_ulong
    lib.XInternAtom.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
    lib.XInternAtom.restype = C.c_ulong
    lib.XChangeProperty.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong, C.c_ulong,
                                    C.c_int, C.c_int, C.c_void_p, C.c_int]
    lib.XCloseDisplay.argtypes = [C.c_void_p]
    display = lib.XOpenDisplay(None)
    assert display
    try:
        yield lib, display
    finally:
        lib.XCloseDisplay(display)


def set_window_class(win, name, instance=None, pid=None):
    with x_connection() as (lib, display):
        value = f"{instance or name}\0{name}\0".encode()
        lib.XChangeProperty(display, win, 67, 31, 8, 0, value, len(value))
        if pid is not None:
            owner = lib.XInternAtom(display, b"_NET_WM_PID", 0)
            role = lib.XInternAtom(display, b"WM_WINDOW_ROLE", 0)
            lib.XChangeProperty(display, win, owner, 6, 32, 0, C.byref(C.c_ulong(pid)), 1)
            lib.XChangeProperty(display, win, role, 31, 8, 0, b"browser", 7)


def check_mwm(tmp, bin_dir, manager_window):
    with application(LAUNCH, "mwm") as app:
        out, err = app.communicate(timeout=4)
        assert app.returncode == 1 and not out and "requires a running mwm" in err, (out, err)

    with x_connection() as (lib, display):
        check = lib.XInternAtom(display, b"_NET_SUPPORTING_WM_CHECK", 0)
        name = lib.XInternAtom(display, b"_NET_WM_NAME", 0)
        utf8 = lib.XInternAtom(display, b"UTF8_STRING", 0)
        root = lib.XDefaultRootWindow(display)
        manager = C.c_ulong(manager_window)
        lib.XChangeProperty(display, root, check, 33, 32, 0, C.byref(manager), 1)
        lib.XChangeProperty(display, manager_window, name, utf8, 8, 0, b"mwm", 3)

    calls = tmp / "mwm-arguments"
    client = bin_dir / "mwmc"
    client.write_text(
        "#!/bin/sh\n"
        'case "$*" in\n'
        '  "query tags") printf "tag 0 Home mon=0 sel=1 occ=1 layout=[]=\\n'
        'tag 3 Project Lab mon=1 sel=1 occ=0 layout=[]=\\n";;\n'
        '  "query monitors") printf "mon 0 1280x800+0+0 seltag=0 sel=1\\n'
        'mon 1 1024x768+1280+0 seltag=3 sel=0\\n";;\n'
        '  reload) printf "error: cannot reload\\n"; exit 1;;\n'
        f'  *) printf "<%s>\\n" "$@" > "{calls}";;\n'
        'esac\n'
    )
    client.chmod(0o755)

    with application(LAUNCH) as app:
        window("Launch", app)
        type_text("mwm")
        keys("Return", "Escape", "Return")
        type_text("move window to tag Project Lab")
        keys("Return")
        check_exit(app, "")
    wait_file(calls, "<move>\n<win>\n<4>\n")

    for query, expected in (
        ("move current tag to monitor 1", "<move>\n<tag>\n<1>\n"),
        ("move window to next monitor", "<move>\n<win>\n<mon>\n<next>\n"),
        ("mwmc focus tag 'Project Lab'", "<focus>\n<tag>\n<Project Lab>\n"),
    ):
        with application(LAUNCH, "mwm") as app:
            window("Launch", app)
            type_text(query)
            keys("Return")
            check_exit(app, "")
        wait_file(calls, expected)

    with application(LAUNCH, "mwm") as app:
        window("Launch", app)
        type_text("reload mwm")
        keys("Return")
        out, err = app.communicate(timeout=4)
        assert (app.returncode, out, err) == (1, "", "launch: error: cannot reload\n")


def check_theme(ini):
    def resources(values):
        subprocess.run(["xprop", "-root", "-f", "RESOURCE_MANAGER", "8s",
                        "-set", "RESOURCE_MANAGER", values], check=True, timeout=3)

    def frame(win, height, shaped):
        info = subprocess.check_output(["xwininfo", "-id", str(win), "-shape"], text=True, timeout=3)
        assert f"Height: {height}\n" in info, info
        assert ("Window shape extents:" in info) == shaped, info
        keys("Escape")

    desktop = "mwm.statusbg: #120000\nmwm.statusfg: #feebea\nmwm.selbg: #e568d0\n"
    minimal = "[launch]\nstartup=minimal\nfont=Sans 12\n"
    ini.write_text(minimal)
    resources(desktop)
    menu([LAUNCH], None, lambda win: frame(win, 56, True), "", 1)
    resources(desktop + "launch.cornerRadius: 0\nlaunch.borderWidth: 3\n")
    menu([LAUNCH], None, lambda win: frame(win, 60, False), "", 1)
    ini.write_text(minimal + "corner-radius=6\nborder-width=2\n")
    menu([LAUNCH], None, lambda win: frame(win, 58, True), "", 1)


def accept_file(app, key):
    end = time.monotonic() + 4
    while time.monotonic() < end and app.poll() is None:
        keys(key)
        time.sleep(0.06)


def files_and_web(tmp, ini, bin_dir):
    root = tmp / "search"
    reports = root / "Archive"
    reports.mkdir(parents=True)
    report = reports / "zqx-report-2025.txt"
    report.write_text("test file")
    for directory in (root / ".private", root / "node_modules"):
        directory.mkdir()
        (directory / "hidden-me.txt").touch()
    (root / "linked-me.txt").symlink_to(report)

    opened = tmp / "opened-file"
    exported_url = tmp / "opened-web"
    revealed = tmp / "revealed-file"
    for name, target in (("xdg-open", opened), ("browser-stub", exported_url), ("dirlane-stub", revealed)):
        command = bin_dir / name
        command.write_text(f"#!/bin/sh\nprintf '%s\\n' \"$@\" > '{target}'\n")
        command.chmod(0o755)
    config = f"""[launch]
border-glow=false
[files]
roots={root};
reveal-command=dirlane-stub --reveal
[browser personal]
window-class=LaunchTestPersonal
command=browser-stub personal
[browser work]
window-class=LaunchTestWork
command=browser-stub work
[browser closed]
window-class=LaunchTestClosed
command=browser-stub closed
[bangs]
site=https://example.test/?q={{searchTerms}}
w=
"""
    ini.write_text(config)
    with application(LAUNCH) as app:
        window("Launch", app)
        type_text("zqx rprt 2025")
        accept_file(app, "Return")
        check_exit(app, "")
    wait_file(opened, f"{report}\n")
    with application(LAUNCH) as app:
        window("Launch", app)
        type_text("zqx rprt 2025")
        accept_file(app, "alt+Return")
        check_exit(app, "")
    wait_file(revealed, f"--reveal\n{report}\n")

    with application(LAUNCH) as app:
        window("Launch", app)
        type_text("files")
        keys("Return")
        type_text("archive zqx")
        accept_file(app, "ctrl+Return")
        check_exit(app, "")
    assert clipboard() == str(report)
    index, = (tmp / "cache" / "launch").glob("files-*.index")
    assert index.stat().st_mode & 0o777 == 0o600
    data = index.read_bytes()
    assert os.fsencode(report) in data
    assert all(name not in data for name in (b".private", b"node_modules", b"linked-me.txt"))

    ini.write_text(config.replace("[files]", "[files]\nfallback=false"))
    outside = tmp / "Outside search roots"
    outside.mkdir()
    document = outside / "draft !site.txt "
    document.touch()
    shortcut = tmp / "Folder shortcut"
    shortcut.symlink_to(outside)
    for query, argv, target, expected in (
        (str(outside), (), opened, f"{outside}\n"),
        (str(shortcut), ("files",), opened, f"{shortcut}\n"),
        (str(document), (), revealed, f"--reveal\n{document}\n"),
    ):
        target.unlink(missing_ok=True)
        subprocess.run(["xclip", "-selection", "clipboard"], input=query, text=True,
                       check=True, timeout=3)
        with application(LAUNCH, *argv) as app:
            window("Launch", app)
            keys("ctrl+v")
            time.sleep(0.1)
            assert app.poll() is None and not target.exists()
            keys("Return")
            check_exit(app, "")
        wait_file(target, expected)

    with application(LAUNCH) as app:
        window("Launch", app)
        type_text("zqx-report")
        keys("Return")
        assert app.poll() is None
        keys("ctrl+a")
        type_text("files archive/zqx")
        accept_file(app, "ctrl+Return")
        check_exit(app, "")
    assert clipboard() == str(report)
    with application(LAUNCH, "files") as app:
        window("Launch", app)
        type_text("zqx-report")
        accept_file(app, "ctrl+Return")
        check_exit(app, "")
    assert clipboard() == str(report)

    with application("xev", "-name", "Launch test personal") as personal:
        set_window_class(window("Launch test personal", personal), "LaunchTestPersonal")
        with application("xev", "-name", "Launch test work") as work:
            set_window_class(window("Launch test work", work), "LaunchTestWork")
            for query, expected in [
                ("how tall is mt. everest !gai",
                 "personal\nhttps://www.google.com/search?q=how+tall+is+mt.+everest&udm=50\n"),
                ("how tall is mt. everest !gai@work",
                 "work\nhttps://www.google.com/search?q=how+tall+is+mt.+everest&udm=50\n"),
                ("!site C++ & Rust @personal", "personal\nhttps://example.test/?q=C%2B%2B+%26+Rust\n"),
                ("https://example.test/path @work", "work\nhttps://example.test/path\n"),
                ("!gh", "personal\nhttps://github.com/\n"),
            ]:
                menu([LAUNCH], None, lambda _: (type_text(query), keys("Return")), "")
                wait_file(exported_url, expected)
                exported_url.unlink()
            for query in ("test !g@closed", "test !g@unset", "!w encyclopedia"):
                menu([LAUNCH], None, lambda _: (type_text(query), keys("Return", "Escape")), "", 1)
                assert not exported_url.exists()

    profile = tmp / "profile-without-links"
    profile.mkdir()
    inbox = tmp / "SingletonSocket"
    focused = tmp / "focused-browser"
    with ini.open("a") as settings:
        settings.write(f"\n[browser recovered]\nwindow-class=LaunchRecovered\n"
                       f"command=browser-stub --user-data-dir={profile} --profile-directory=Default\n"
                       f"focus-command=touch '{focused}'\n")
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(str(inbox))
        listener.listen(1)
        listener.settimeout(4)
        with application("xev", "-name", "Launch test recovered") as browser:
            win = window("Launch test recovered", browser)
            set_window_class(win, "LaunchRecovered", f"helium ({profile})", os.getpid())
            with application(LAUNCH) as app:
                window("Launch", app)
                type_text("socket recovery !site@recovered")
                keys("Return")
                with listener.accept()[0] as connection:
                    connection.settimeout(3)
                    payload = bytearray()
                    while chunk := connection.recv(4096):
                        payload.extend(chunk)
                    assert payload.split(b"\0") == [
                        b"START", os.fsencode(Path.cwd()), b"chromium",
                        b"--profile-directory=Default", b"https://example.test/?q=socket+recovery",
                    ]
                    connection.sendall(b"ACK")
                check_exit(app, "")
                assert not exported_url.exists()
                wait_file(focused, "")


def main(tmp):
    config = tmp / "config" / "launch"
    apps = tmp / "data" / "applications"
    bin_dir = tmp / "bin"
    empty = tmp / "empty"
    for path in (config, apps, bin_dir, empty):
        path.mkdir(parents=True)
    os.environ.update(XDG_CONFIG_HOME=str(config.parent), XDG_DATA_HOME=str(apps.parent),
                      XDG_DATA_DIRS=str(empty), XDG_CURRENT_DESKTOP="mwm",
                      XDG_STATE_HOME=str(tmp / "state"), XDG_CACHE_HOME=str(tmp / "cache"))
    os.environ["PATH"] = str(bin_dir) + os.pathsep + os.environ["PATH"]
    app_result = tmp / "desktop-result"
    command_result = tmp / "command-result"
    workflow_result = tmp / "workflow-result"
    example_app = bin_dir / "launch-example"
    example_app.write_text(f"#!/bin/sh\nprintf app > '{app_result}'\n")
    example_app.chmod(0o755)
    command = bin_dir / "launch-custom-terminal-command"
    command.write_text(f"#!/bin/sh\nprintf command > '{command_result}'\n")
    command.chmod(0o755)
    # The explicit workflow alias wins even when a same-named command is on PATH.
    terminal_calc = bin_dir / "calc"
    terminal_calc.write_text("#!/bin/sh\nexit 7\n")
    terminal_calc.chmod(0o755)
    (apps / "example.desktop").write_text(
        "[Desktop Entry]\nType=Application\nName=Example Browser\n"
        f"Exec={example_app}\nKeywords=web;internet;\n"
    )
    (config / "config.ini").write_text(
        "[dmenu]\nrow-height=35\n[keys]\naccept=Ctrl+F6\n"
        f"[workflow Notes]\nkeywords=journal diary\ncommand=touch '{workflow_result}'\n"
        "[workflow Direct]\ncommand=printf '%s:%s' prefix\n"
    )

    def vertical(win):
        info = xdotool("getwindowgeometry", win).stdout
        assert "Geometry: 1280x140" in info, info
        type_text("fir")
        keys("Down", "Return")

    menu([DMENU, "-i", "-l", "10", "-p", "Select:"],
         "Notepad\nFire Fox\nFirefox Nightly\n", vertical, "Firefox Nightly\n")
    menu([DMENU], "First\nSecond\n", lambda _: keys("Right", "Return"), "Second\n")
    menu([DMENU, "-f", "-l", "2"], "First\nSecond\n",
         lambda _: keys("ctrl+Return", "Down", "Return"), "First\nSecond\n")
    menu([DMENU, "-l", "2"], "First\nSecond\n",
         lambda _: (type_text("Fi"), keys("Tab", "shift+Return")), "First\n")
    menu([DMENU], "", lambda _: (type_text("typed text"), keys("shift+Return")), "typed text\n")
    menu([DMENU], "abort\n", lambda _: keys("Escape"), "", 1)
    menu([DMENU, "-l", "4"], "tail prefix\nprefix tail extra\nprefix tail\n",
         lambda _: (type_text("prefix tail"), keys("Return")), "prefix tail\n")
    menu([LAUNCH, "pick"], "one\nCafé\n",
         lambda _: (type_text("cafe"), keys("Return")), "Café\n")
    menu([LAUNCH, "pick"], "shortcut\n", lambda _: keys("ctrl+F6"), "shortcut\n")
    menu([LAUNCH, "pick", "--literal"], "literal\n",
         lambda _: (type_text("ltrl"), keys("Return")), "ltrl\n")

    def edit_field(_):
        type_text("one two three")
        keys("ctrl+Left", "ctrl+Left")
        type_text("X")
        keys("ctrl+Right")
        type_text("Y")
        keys("Home", "ctrl+e")
        type_text("A")
        keys("End")
        type_text("W")
        keys("shift+Return")

    menu([LAUNCH, "pick"], "", edit_field, "Aone XtwoY threeW\n")
    menu([LAUNCH, "pick"], "", lambda _: (type_text("replace this"), keys("ctrl+a"),
         type_text("replacement"), keys("shift+Return")), "replacement\n")
    with application(LAUNCH) as app:
        window("Launch", app)
        xdotool("keydown", "Super_L", "keydown", "space").check_returncode()
        time.sleep(0.1)
        assert app.poll() is None
        xdotool("keyup", "Super_L", "keyup", "space").check_returncode()
        check_exit(app, "", 1)
    with (config / "config.ini").open("a") as file:
        file.write("\n[launch]\ntoggle=false\n")
    with application(LAUNCH) as app:
        window("Launch", app)
        keys("super+space")
        time.sleep(0.1)
        assert app.poll() is None
        keys("Escape")
        check_exit(app, "", 1)
    with (config / "config.ini").open("a") as file:
        file.write("toggle=true\n[keys]\ntoggle=Mod4+F7\n")
    with application(LAUNCH) as app:
        window("Launch", app)
        keys("super+F7")
        check_exit(app, "", 1)

    menu([LAUNCH, "pick", "--require-match"], "known\n",
         lambda _: (type_text("unknown"), keys("Return", "Escape")), "", 1)
    menu([LAUNCH, "pick", "--require-match"], "known\n",
         lambda _: keys("shift+Return", "Return"), "known\n")
    direct = subprocess.run([LAUNCH, "workflow", "direct", "two words"], text=True,
                            capture_output=True, check=True, timeout=3)
    assert (direct.stdout, direct.stderr) == ("prefix:two words", "")

    with application(LAUNCH) as app:
        window("Launch", app)
        type_text("emoj")
        keys("Return")
        type_text("sweat smile")
        keys("Return")
        check_exit(app, "")
    assert clipboard() == "😅"

    menu([LAUNCH, "emoji", "--print"], None, lambda _: keys("Right", "Return"), "😃\n")
    paste = subprocess.run(["xclip", "-selection", "clipboard"], input="copy me", text=True,
                           capture_output=False, check=True, timeout=3)
    assert paste.returncode == 0
    menu([DMENU], "", lambda _: (keys("ctrl+shift+y"), time.sleep(0.1), keys("Return")), "copy me\n")
    menu([LAUNCH, "calc", "--print"], None,
         lambda _: (type_text("2^3^2 + (24/4)"), keys("Return")), "518\n")
    menu([LAUNCH, "calc", "--print"], None,
         lambda _: (type_text("20/0"), keys("Return", "Escape")), "", 1)

    with application(LAUNCH) as app:
        window("Launch", app)
        type_text("emj")
        keys("Return", "Escape", "ctrl+u")
        type_text("calc")
        keys("Return")
        type_text("(18 + 6) / 4")
        keys("Return")
        check_exit(app, "")
    assert clipboard() == "6"

    for query, result, expected in [
        ("ex brws", app_result, "app"),
        ("launch-custom-terminal-command", command_result, "command"),
        ("diary", workflow_result, ""),
    ]:
        with application(LAUNCH) as app:
            window("Launch", app)
            type_text(query)
            keys("Return")
            check_exit(app, "")
        wait_file(result, expected)

    for _ in range(2):
        app_result.unlink()
        with application(LAUNCH) as app:
            window("Launch", app)
            type_text("ex brws")
            keys("Return")
            check_exit(app, "")
        wait_file(app_result, "app")
    app_result.unlink()
    menu([LAUNCH], None, lambda _: keys("Return"), "")
    wait_file(app_result, "app")

    ini = config / "config.ini"
    for startup in ("minimal", "clock"):
        ini.write_text(f"[launch]\nstartup={startup}\nborder-glow=false\n")
        with application(LAUNCH) as app:
            win = window("Launch", app)
            before = xdotool("getwindowgeometry", win).stdout
            type_text("sys")
            query = wait_geometry(win, lambda geometry: geometry != before)
            keys("Return", "Right", "Tab", "5")
            # Escape returns to the existing query. Enter reopens System.
            keys("Escape")
            wait_geometry(win, lambda geometry: geometry == query)
            keys("Return", "Escape", "ctrl+a", "BackSpace")
            wait_geometry(win, lambda geometry: geometry == before)
            keys("Escape")
            check_exit(app, "", 1)
    ini.write_text("[launch]\ncorner-radius=0\n[system]\ninterval-ms=500\n")
    menu([LAUNCH, "system"], None, lambda _: keys("Up", "Down", "Right", "5", "Escape"), "", 1)

    with application(LAUNCH) as app:
        win = window("Launch", app)

        def expect_height(height):
            wait_geometry(win, lambda geometry: f"Geometry: 672x{height}\n" in geometry)

        type_text("calend")
        keys("Return")
        expect_height(459)
        keys("Left")
        expect_height(609)
        keys("Up", "t", "Right")
        expect_height(459)
        keys("Right", "Down", "Right", "Up", "Right", "t")
        expect_height(373)
        keys("Escape", "Return")
        expect_height(459)
        keys("Escape", "Escape")
        check_exit(app, "", 1)
    menu([LAUNCH, "calendar"], None, lambda _: keys("Down", "t", "Escape"), "", 1)
    files_and_web(tmp, ini, bin_dir)
    check_conversions(tmp, bin_dir)

    with application("xev", "-name", "Launch embed target") as parent:
        embed = window("Launch embed target", parent)
        menu([DMENU, "-w", str(embed)], "embedded\n", lambda _: keys("Return"), "embedded\n")
        check_theme(ini)
        if not os.environ.get("TEST_WM"):
            check_mwm(tmp, bin_dir, embed)

    print("X11 integration checks passed")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="launch-test-") as directory:
        wm = os.environ.get("TEST_WM")
        if wm:
            assert shutil.which(wm), f"window manager {wm!r} unavailable"
            with application(wm) as manager:
                time.sleep(0.1)
                assert manager.poll() is None
                main(Path(directory))
        else:
            main(Path(directory))
