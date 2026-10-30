"""Board-local EdgeVision lifecycle and real measurements; no credentials printed."""
import argparse
import csv
import datetime
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import time
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
STATE = ROOT / 'local' / 'session.json'
ACTIVE_PROFILE = ROOT / 'local' / 'active-profile.json'


def profile_catalog():
    return json.loads((ROOT / 'ema3588/model_profiles.json').read_text())


def active_profile():
    if ACTIVE_PROFILE.exists():
        return json.loads(ACTIVE_PROFILE.read_text())['profile']
    if all((ROOT/'ema-runtime/models'/kind/'model.rknn').is_file() for kind in ('coco80','helmet')):
        return 'indoor-helmet'
    return 'legacy'


def profile_environment(name):
    profiles = profile_catalog()
    if name not in profiles:
        raise ValueError('Unknown model profile: ' + name)
    config = profiles[name]
    runtime = (ROOT / 'ema-runtime').resolve()
    environment = os.environ.copy()
    for key in ('EDGEVISION_AUX_MODEL', 'EDGEVISION_AUX_LABELS', 'EDGEVISION_AUX_CONFIDENCE', 'EDGEVISION_AUX_CLASS'):
        environment.pop(key, None)
    for key, variable in (('model', 'EDGEVISION_MODEL'), ('labels', 'EDGEVISION_LABELS'),
                          ('aux_model', 'EDGEVISION_AUX_MODEL'), ('aux_labels', 'EDGEVISION_AUX_LABELS')):
        if key not in config:
            continue
        path = (runtime / config[key]).resolve()
        if runtime not in path.parents or not path.is_file():
            raise RuntimeError('Missing or invalid model profile file: ' + str(path))
        environment[variable] = str(path)
    environment['EDGEVISION_APP_DIR'] = str(runtime)
    environment['EDGEVISION_CONFIDENCE'] = str(config['confidence'])
    environment['EDGEVISION_NPU_CORES'] = str(config['npu_core_mask'])
    if 'aux_model' in config:
        environment['EDGEVISION_AUX_CONFIDENCE'] = str(config['aux_confidence'])
        environment['EDGEVISION_AUX_CLASS'] = str(config['aux_class'])
    return environment


def write_state(value):
    STATE.parent.mkdir(exist_ok=True)
    STATE.chmod(0o600) if STATE.exists() else None
    temporary = STATE.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2))
    temporary.chmod(0o600)
    temporary.replace(STATE)


def load_state():
    return json.loads(STATE.read_text()) if STATE.exists() else {}


def api(path, post=False):
    token = Path('/userdata/uav-object-detection/data/admin-token.txt').read_text().strip()
    request = urllib.request.Request('http://127.0.0.1:8088/api/' + path,
        headers={'Authorization': 'Bearer ' + token}, data=b'' if post else None)
    with urllib.request.urlopen(request, timeout=20) as response:
        return json.load(response)


def owners():
    value = subprocess.run(['lsof', '-t', '/dev/video44'], stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, text=True)
    return sorted(set(int(item) for item in value.stdout.split() if item.isdigit()))


def process_identity(pid):
    try:
        raw = Path('/proc/{}/stat'.format(pid)).read_text().rsplit(')', 1)[1].split()
        if raw[0] == 'Z':
            return None
        return raw[19]  # field 22, kernel monotonic start ticks
    except (FileNotFoundError, ProcessLookupError):
        return None


def alive(state):
    return bool(state.get('pid') and process_identity(state['pid']) == state.get('start_ticks'))


def stream_diagnostics(log_path, running):
    """Bounded log inspection; historical failures never mark a new session failed."""
    result = {'phase': 'running' if running else 'stopped', 'error': None,
              'last_metric': None, 'metrics_stale': True}
    if not log_path:
        return result
    try:
        with Path(log_path).open('rb') as log:
            log.seek(0, os.SEEK_END)
            log.seek(max(0, log.tell() - 65536))
            lines = log.read().decode('utf-8', errors='replace').splitlines()
    except OSError:
        return result
    for line in lines:
        if line.startswith('METRIC '):
            try:
                result['last_metric'] = json.loads(line[7:])
            except ValueError:
                pass
        if not running and ('failed to submit' in line or 'rknn_run fail' in line):
            result.update(phase='failed', error={'code': 'rknn_inference_failed',
                'message': 'RKNN task submission/inference failed; inspect the session log.'})
        elif not running and 'inference_yolov8_model fail' in line and result['error'] is None:
            result.update(phase='failed', error={'code': 'inference_failed', 'message': line.strip()})
    # Old logs lack per-METRIC timestamps. Consumers must compare frame counts
    # across polls to establish progress; log mtime cannot prove fresh frames.
    result['metrics_stale'] = True if not running or result['last_metric'] is None else None
    return result


def restore(state):
    if state.get('uav_stopped_by_us'):
        if owners():
            raise RuntimeError('Camera is still busy; original application was not restarted.')
        if api('settings') != state['uav_settings']:
            raise RuntimeError('UAV settings changed during the test; inspect before restoring.')
        api('inference/start', post=True)
        state['uav_stopped_by_us'] = False
        state['uav_restored_at'] = time.time()
        write_state(state)


def stop(restore_uav=False):
    state = load_state()
    if alive(state):
        os.killpg(state['pid'], signal.SIGTERM)
        deadline = time.monotonic() + 10
        while alive(state) and time.monotonic() < deadline:
            time.sleep(.1)
        if alive(state):
            os.killpg(state['pid'], signal.SIGKILL)
            time.sleep(.2)
        if alive(state):
            raise RuntimeError('EdgeVision did not exit.')
    state['pid'] = None
    state['stopped_at'] = time.time()
    write_state(state)
    if restore_uav:
        restore(state)
    print(json.dumps({'stopped': True, 'uav_restored': not state.get('uav_stopped_by_us', False)}))


def start(take_camera=False, profile=None, restore_on_failure=True):
    profile = profile or active_profile()
    state = load_state()
    if alive(state):
        if state.get('profile', 'legacy') != profile:
            raise RuntimeError('A different profile is running. Use apply-profile --profile ' + profile)
        print(json.dumps({'already_running': True, 'pid': state['pid'], 'profile': profile}))
        return
    environment = profile_environment(profile)
    runtime = ROOT / 'ema-runtime'
    subprocess.run(['sha256sum', '--quiet', '-c', 'SHA256SUMS'], cwd=str(runtime), check=True)
    with socket.socket() as check:
        check.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        check.bind(('0.0.0.0', 8080))
    camera_pids = owners()
    if camera_pids:
        if not take_camera:
            raise RuntimeError('Camera busy, PIDs {}. Use --take-camera for controlled UAV handoff.'.format(camera_pids))
        for pid in camera_pids:
            if 'uav-object-detection.service' not in Path('/proc/{}/cgroup'.format(pid)).read_text():
                raise RuntimeError('Camera owner {} is outside the known UAV service; refusing to stop it.'.format(pid))
        previous = api('status')
        state = {'uav_settings': api('settings'), 'uav_previous_phase': previous.get('phase'),
                 'uav_was_running': bool(previous.get('worker_pid')), 'uav_stopped_by_us': True,
                 'created_at': time.time()}
        write_state(state)
        api('inference/stop', post=True)
        deadline = time.monotonic() + 12
        while owners() and time.monotonic() < deadline:
            time.sleep(.2)
    try:
        if owners():
            raise RuntimeError('Camera did not become free after the controlled stop.')
        (ROOT / 'local').mkdir(exist_ok=True)
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        log = ROOT / 'local' / ('stream-' + stamp + '.log')
        with log.open('wb') as output:
            process = subprocess.Popen(['bash', str(ROOT / 'ema3588/start_imx415_mjpeg.sh')],
                stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                start_new_session=True, env=environment)
        state.update(pid=process.pid, start_ticks=process_identity(process.pid),
                     log=str(log), started_at=time.time(), profile=profile)
        write_state(state)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError('EdgeVision exited: ' + log.read_text(errors='replace')[-5000:])
            try:
                with urllib.request.urlopen('http://127.0.0.1:8080/', timeout=2) as response:
                    block = response.read(16384)
                    if b'\xff\xd8' in block:
                        ACTIVE_PROFILE.write_text(json.dumps({'profile': profile}))
                        print(json.dumps({'running': True, 'pid': process.pid,
                            'profile': profile, 'url': 'http://192.168.2.221:8080/', 'log': str(log)}))
                        return
            except (OSError, TimeoutError):
                time.sleep(.2)
        raise RuntimeError('No live JPEG frame received within 30 seconds.')
    except BaseException:
        stop(restore_uav=restore_on_failure)
        raise


def apply_profile(profile, take_camera=False):
    profile_environment(profile)  # Validate files before interrupting the old stream.
    state = load_state()
    was_running = alive(state)
    previous = state.get('profile', active_profile())
    if was_running and previous == profile:
        return start(profile=profile)
    preview_script=ROOT/'ema3588/open_hdmi_preview.py'
    preview_state=ROOT/'local/hdmi-preview.json'
    had_preview=False
    if preview_state.exists():
        value=json.loads(preview_state.read_text())
        had_preview=alive(value)
    if had_preview:
        subprocess.run(['python3',str(preview_script),'--stop'],check=True)
    if was_running:
        stop()
    try:
        start(take_camera=take_camera, profile=profile, restore_on_failure=not was_running)
        if had_preview:
            subprocess.run(['python3',str(preview_script)],check=True)
    except BaseException:
        if was_running:
            if alive(load_state()):
                stop()
            if had_preview:
                subprocess.run(['python3',str(preview_script),'--stop'],check=False)
            start(profile=previous, restore_on_failure=True)
            if had_preview:
                subprocess.run(['python3',str(preview_script)],check=False)
            print(json.dumps({'restored_profile': previous}))
        raise


def resources(pid):
    values = {}
    for row in Path('/proc/meminfo').read_text().splitlines():
        key, value = row.split(':', 1)
        values[key] = int(value.split()[0])
    npu_text = Path('/sys/kernel/debug/rknpu/load').read_text()
    result = {'memory_pct': 100 * (1 - values['MemAvailable'] / values['MemTotal'])}
    for core, value in re.findall(r'Core(\d):\s*(\d+)%', npu_text):
        result['npu{}_pct'.format(core)] = int(value)
    proc = Path('/proc/{}/stat'.format(pid)).read_text().rsplit(')', 1)[1].split()
    result['process_ticks'] = int(proc[11]) + int(proc[12])
    result['rss_mb'] = int(proc[21]) * os.sysconf('SC_PAGE_SIZE') / 1024**2
    cpu = list(map(int, Path('/proc/stat').read_text().splitlines()[0].split()[1:9]))
    result['cpu_total'] = sum(cpu)
    result['cpu_idle'] = cpu[3] + cpu[4]
    return result


def sample(seconds):
    state = load_state()
    if not alive(state):
        raise RuntimeError('EdgeVision is not running.')
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    path = ROOT / 'local' / ('telemetry-' + stamp + '.csv')
    fields = ['at', 'frames', 'fps', 'detect_ms', 'objects', 'board_cpu_pct',
              'process_cpu_pct', 'rss_mb', 'memory_pct', 'npu0_pct', 'npu1_pct', 'npu2_pct']
    previous = resources(state['pid'])
    last_time = time.monotonic()
    deadline = last_time + seconds
    rows = []
    log_position = 0
    metric = {}
    with path.open('w', newline='') as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        while time.monotonic() < deadline:
            time.sleep(min(1, max(0, deadline - time.monotonic())))
            if not alive(state):
                raise RuntimeError('EdgeVision stopped during sampling.')
            now = time.monotonic()
            current = resources(state['pid'])
            with Path(state['log']).open() as log:
                log.seek(log_position)
                for line in log:
                    if line.startswith('METRIC '):
                        metric = json.loads(line[7:])
                log_position = log.tell()
            total_delta = current['cpu_total'] - previous['cpu_total']
            row = dict(at=time.time(), **{k: metric.get(k) for k in ('frames', 'fps', 'detect_ms', 'objects')})
            row.update({key: current.get(key) for key in ('rss_mb', 'memory_pct', 'npu0_pct', 'npu1_pct', 'npu2_pct')})
            row['board_cpu_pct'] = 100 * (1 - (current['cpu_idle'] - previous['cpu_idle']) / total_delta) if total_delta else None
            row['process_cpu_pct'] = 100 * (current['process_ticks'] - previous['process_ticks']) / os.sysconf('SC_CLK_TCK') / (now - last_time)
            writer.writerow(row)
            output.flush()
            rows.append(row)
            previous, last_time = current, now
            if len(rows) % 10 == 0:
                print(json.dumps({'samples': len(rows), 'latest': row}), flush=True)
    summary = {'seconds_requested': seconds, 'samples': len(rows), 'csv': str(path),
               'process_cpu_units': 'one logical CPU = 100%', 'metrics': {}}
    for key in ('fps', 'detect_ms', 'board_cpu_pct', 'process_cpu_pct', 'rss_mb', 'memory_pct', 'npu0_pct', 'npu1_pct', 'npu2_pct'):
        data = [row[key] for row in rows if isinstance(row.get(key), (int, float))]
        summary['metrics'][key] = {'mean': sum(data) / len(data), 'min': min(data), 'max': max(data)} if data else None
    path.with_suffix('.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=('start', 'stop', 'status', 'sample', 'profiles', 'apply-profile'))
    parser.add_argument('--take-camera', action='store_true')
    parser.add_argument('--restore-uav', action='store_true')
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--profile', choices=sorted(profile_catalog()))
    args = parser.parse_args()
    if args.action == 'start':
        start(args.take_camera, args.profile)
    elif args.action == 'stop':
        stop(args.restore_uav)
    elif args.action == 'sample':
        if not 1 <= args.seconds <= 1800:
            parser.error('--seconds must be between 1 and 1800')
        sample(args.seconds)
    elif args.action == 'profiles':
        print(json.dumps({'active': active_profile(), 'profiles': profile_catalog()}))
    elif args.action == 'apply-profile':
        if not args.profile:
            parser.error('apply-profile requires --profile')
        apply_profile(args.profile, args.take_camera)
    else:
        state = load_state()
        running = alive(state)
        print(json.dumps({'running': running, 'pid': state.get('pid'),
            'camera_owners': owners(), 'log': state.get('log'),
            'profile': state.get('profile', 'legacy'),
            'uav_paused_for_edgevision': state.get('uav_stopped_by_us', False),
            **stream_diagnostics(state.get('log'), running)}))


if __name__ == '__main__':
    main()
