"""Place only this application's preview window on the primary X11 monitor."""
import ctypes as c
import os
from pathlib import Path
import time


class ClientData(c.Union):
    _fields_ = [('b', c.c_char * 20), ('s', c.c_short * 10), ('l', c.c_long * 5)]


class ClientMessage(c.Structure):
    _fields_ = [('type', c.c_int), ('serial', c.c_ulong), ('send_event', c.c_int),
        ('display', c.c_void_p), ('window', c.c_ulong), ('message_type', c.c_ulong),
        ('format', c.c_int), ('data', ClientData)]


class XEvent(c.Union):
    _fields_ = [('client', ClientMessage), ('padding', c.c_long * 24)]


def place_preview(identity, environment, timeout=30):
    for key in ('DISPLAY', 'XAUTHORITY'):
        if key in environment:
            os.environ[key] = environment[key]
    x = c.CDLL('libX11.so.6')
    x.XOpenDisplay.argtypes = [c.c_char_p]
    x.XOpenDisplay.restype = c.c_void_p
    x.XDefaultRootWindow.argtypes = [c.c_void_p]
    x.XDefaultRootWindow.restype = c.c_ulong
    x.XInternAtom.argtypes = [c.c_void_p, c.c_char_p, c.c_int]
    x.XInternAtom.restype = c.c_ulong
    x.XGetWindowProperty.argtypes = [c.c_void_p, c.c_ulong, c.c_ulong, c.c_long,
        c.c_long, c.c_int, c.c_ulong, c.POINTER(c.c_ulong), c.POINTER(c.c_int),
        c.POINTER(c.c_ulong), c.POINTER(c.c_ulong), c.POINTER(c.POINTER(c.c_ubyte))]
    x.XFree.argtypes = [c.c_void_p]
    x.XSendEvent.argtypes = [c.c_void_p, c.c_ulong, c.c_int, c.c_long, c.POINTER(XEvent)]
    x.XSync.argtypes = [c.c_void_p, c.c_int]
    x.XCloseDisplay.argtypes = [c.c_void_p]
    display = x.XOpenDisplay(None)
    if not display:
        raise RuntimeError('Could not access the desktop X11 display.')
    root = x.XDefaultRootWindow(display)

    def atom(name):
        return x.XInternAtom(display, name.encode(), 0)

    def prop(window, name):
        kind, fmt, count, after = c.c_ulong(), c.c_int(), c.c_ulong(), c.c_ulong()
        data = c.POINTER(c.c_ubyte)()
        x.XGetWindowProperty(display, window, atom(name), 0, 4096, 0, 0,
            c.byref(kind), c.byref(fmt), c.byref(count), c.byref(after), c.byref(data))
        if not data:
            return []
        try:
            return list(c.cast(data, c.POINTER(c.c_ulong))[:count.value]) if fmt.value == 32 else []
        finally:
            x.XFree(data)

    def message(window, name, values):
        event = XEvent()
        event.client.type = 33
        event.client.display = display
        event.client.window = window
        event.client.message_type = atom(name)
        event.client.format = 32
        for index, value in enumerate(values):
            event.client.data.l[index] = value
        if not x.XSendEvent(display, root, 0, (1 << 19) | (1 << 20), c.byref(event)):
            raise RuntimeError('The window manager rejected the placement request.')

    try:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for window in prop(root, '_NET_CLIENT_LIST'):
                pids = prop(window, '_NET_WM_PID')
                if not pids:
                    continue
                try:
                    command = Path('/proc/{}/cmdline'.format(pids[0])).read_bytes().split(b'\0')
                except OSError:
                    continue
                if str(identity).encode() not in command:
                    continue
                # Move to the primary monitor's work area. The vendor desktop
                # also advertises a DSI panel, so saved placement can be invisible.
                areas = prop(root, '_NET_WORKAREA')
                desktop = (prop(root, '_NET_CURRENT_DESKTOP') or [0])[0]
                origin_x = areas[desktop * 4] if len(areas) > desktop * 4 + 3 else 0
                origin_y = areas[desktop * 4 + 1] if len(areas) > desktop * 4 + 3 else 0
                # On this board HDMI-1 is the primary output at 0,0. Workarea
                # dimensions cover both HDMI and DSI: cap to the verified HDMI.
                px, py, width, height = origin_x + 80, origin_y + 40, 1280, 720
                message(window, '_NET_WM_DESKTOP', [desktop, 2])
                message(window, '_NET_WM_STATE', [0, atom('_NET_WM_STATE_MAXIMIZED_HORZ'), atom('_NET_WM_STATE_MAXIMIZED_VERT'), 2])
                message(window, '_NET_MOVERESIZE_WINDOW', [(2 << 12) | (15 << 8), px, py, width, height])
                message(window, '_NET_ACTIVE_WINDOW', [2, 0])
                x.XSync(display, 0)
                return {'window': window, 'player_pid': pids[0], 'requested_geometry': [px, py, width, height]}
            time.sleep(.2)
        raise RuntimeError('Player did not create a preview window within the deadline. Inspect hdmi-preview.log.')
    finally:
        x.XCloseDisplay(display)
