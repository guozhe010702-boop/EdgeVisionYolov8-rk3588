"""Open the local MJPEG stream with the desktop user's software SDL player."""
import json
import argparse
import os
from pathlib import Path
import pwd
import signal
import subprocess
import time
import urllib.request
from x11_preview import place_preview

ROOT = Path(__file__).resolve().parent.parent
URL = 'http://127.0.0.1:8080/'
TITLE = 'EdgeVision IMX415 HDMI'


def find_desktop():
    for item in Path('/proc').iterdir():
        if not item.name.isdigit():
            continue
        try:
            if item.joinpath('comm').read_text().strip() != 'gnome-shell':
                continue
            account = pwd.getpwuid(item.stat().st_uid)
            pairs = item.joinpath('environ').read_bytes().split(b'\0')
            environment = dict(part.decode(errors='replace').split('=', 1) for part in pairs if b'=' in part)
            if environment.get('DISPLAY'):
                return account, environment
        except (OSError, KeyError):
            continue
    raise RuntimeError('No logged-in GNOME X11 desktop found. Log in on the HDMI screen first.')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--stop', action='store_true')
    options = parser.parse_args()
    state_path = ROOT / 'local' / 'hdmi-preview.json'
    if options.stop:
        if state_path.exists():
            state=json.loads(state_path.read_text())
            pid=state.get('placement', {}).get('player_pid')
            if pid:
                try:
                    command=Path('/proc/{}/cmdline'.format(pid)).read_bytes().split(b'\0')
                    if TITLE.encode() not in command or 'ffplay' not in Path(command[0].decode()).name:
                        raise RuntimeError('Stored preview PID belongs to a different process.')
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                except FileNotFoundError:
                    pass
            state['pid']=None
            state['closed_at']=time.time()
            state_path.write_text(json.dumps(state,indent=2))
        print(json.dumps({'preview_stopped':True}))
        return
    account, session = find_desktop()
    if os.geteuid() not in (0, account.pw_uid):
        raise RuntimeError('Run as root or the logged-in desktop user.')
    with urllib.request.urlopen(URL, timeout=5) as response:
        if b'\xff\xd8' not in response.read(16384):
            raise RuntimeError('The HTTP stream did not return a JPEG frame.')
    folder = ROOT / 'local'
    folder.mkdir(exist_ok=True)
    if state_path.exists():
        state = json.loads(state_path.read_text())
        try:
            stat = Path('/proc/{}/stat'.format(state['pid'])).read_text().rsplit(')', 1)[1].split()
            if state.get('backend') == 'ffplay' and stat[0] != 'Z' and stat[19] == state['start_ticks']:
                placement = place_preview(TITLE, session)
                print(json.dumps(dict(state, already_running=True, placement=placement)))
                return
        except (OSError, KeyError):
            pass
    prefix = ['sudo', '-H', '-u', account.pw_name] if os.geteuid() == 0 else []
    environment = os.environ.copy()
    for key in ('LD_LIBRARY_PATH', 'LD_PRELOAD', 'GTK_PATH', 'GTK_MODULES', 'GIO_EXTRA_MODULES'):
        environment.pop(key, None)
    for key in ('DISPLAY', 'XAUTHORITY', 'XDG_RUNTIME_DIR', 'DBUS_SESSION_BUS_ADDRESS'):
        if session.get(key):
            environment[key] = session[key]
    environment.update(HOME=account.pw_dir, USER=account.pw_name, LOGNAME=account.pw_name,
        SDL_VIDEODRIVER='x11', SDL_RENDER_DRIVER='software')
    command = prefix + ['env'] + [key + '=' + environment[key] for key in
        ('DISPLAY', 'XAUTHORITY', 'XDG_RUNTIME_DIR', 'DBUS_SESSION_BUS_ADDRESS') if key in environment]
    command += ['/usr/bin/ffplay', '-hide_banner', '-loglevel', 'warning', '-nostats',
        '-window_title', TITLE, '-f', 'mpjpeg', '-use_wallclock_as_timestamps', '1',
        '-fflags', 'nobuffer', '-flags', 'low_delay', '-framedrop', '-sync', 'ext',
        '-probesize', '32768', '-analyzeduration', '100000', '-an',
        '-x', '1280', '-y', '720', '-left', '100', '-top', '80', URL]
    log = folder / 'hdmi-preview.log'
    with log.open('ab') as output:
        process = subprocess.Popen(command, env=environment, stdin=subprocess.DEVNULL,
            stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
    stat = Path('/proc/{}/stat'.format(process.pid)).read_text().rsplit(')', 1)[1].split()
    state = dict(pid=process.pid, start_ticks=stat[19], desktop_user=account.pw_name,
                 display=environment['DISPLAY'], backend='ffplay', url=URL, log=str(log))
    state_path.write_text(json.dumps(state, indent=2))
    state_path.chmod(0o600)
    time.sleep(2)
    if process.poll() is not None:
        raise RuntimeError('Player exited. Read ' + str(log))
    state['placement'] = place_preview(TITLE, environment)
    state_path.write_text(json.dumps(state, indent=2))
    print(json.dumps(state))


if __name__ == '__main__':
    main()
