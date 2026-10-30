"""Windows/Linux receiver check: decode genuine HTTP MJPEG, save local evidence."""
import argparse
import json
from pathlib import Path
import shutil
import socket
import subprocess
import threading
import time
from urllib.parse import urlsplit


def verify_handshake(url):
    parts = urlsplit(url)
    for fragmented in (False, True, False):
        with socket.create_connection((parts.hostname, parts.port or 80), timeout=5) as client:
            client.settimeout(5)
            request = ('GET {} HTTP/1.1\r\nHost: {}\r\n\r\n'.format(parts.path or '/', parts.netloc)).encode()
            # Regression: TCP accept can precede arrival of the request.
            time.sleep(.2)
            if fragmented:
                client.sendall(request[:8])
                time.sleep(.15)
                client.sendall(request[8:])
            else:
                client.sendall(request)
            received = b''
            while b'\xff\xd8' not in received and len(received) < 65536:
                block = client.recv(4096)
                if not block:
                    break
                received += block
            if not received.startswith(b'HTTP/1.0 200') or b'\xff\xd8' not in received:
                raise RuntimeError('Delayed/fragmented HTTP request did not receive a live JPEG frame.')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', default='http://192.168.2.221:8080/')
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 1800:
        parser.error('--seconds must be between 1 and 1800')
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        raise SystemExit('FFmpeg was not found in PATH.')
    args.output.mkdir(parents=True, exist_ok=True)
    verify_handshake(args.url)
    common = [ffmpeg, '-hide_banner', '-nostdin', '-loglevel', 'warning',
              '-rw_timeout', '8000000', '-i', args.url]
    snapshot = subprocess.run(common + ['-frames:v', '1', '-update', '1', '-y',
        str(args.output / 'camera-frame.jpg')], capture_output=True, timeout=20)
    (args.output / 'snapshot.log').write_bytes(snapshot.stderr)
    if snapshot.returncode:
        raise RuntimeError('Snapshot decode failed; see snapshot.log.')
    started = time.monotonic()
    with (args.output / 'decode.log').open('wb') as errors:
        process = subprocess.Popen(common + ['-an',
            '-fps_mode', 'passthrough', '-progress', 'pipe:1', '-f', 'null', '-'],
            stdout=subprocess.PIPE, stderr=errors, text=True)
        samples = []
        def read_progress():
            packet = {}
            for line in process.stdout:
                key, _, value = line.strip().partition('=')
                packet[key] = value
                if key == 'progress':
                    samples.append(dict(packet, wall_elapsed_s=time.monotonic() - started))
                    packet = {}
        reader = threading.Thread(target=read_progress, daemon=True)
        reader.start()
        wall_timer_reached = False
        try:
            process.wait(timeout=args.seconds)
        except subprocess.TimeoutExpired:
            wall_timer_reached = True
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
            reader.join(timeout=5)
    duration = time.monotonic() - started
    summary = {'exit_code': process.returncode, 'wall_timer_reached': wall_timer_reached,
               'termination_note': 'Null-output decoder is terminated by the wall timer; no recording needs finalization.',
               'wall_seconds': duration,
               'samples': samples, 'snapshot': str(args.output / 'camera-frame.jpg')}
    if samples:
        summary['decoded_frames'] = int(samples[-1].get('frame', 0))
        if len(samples) > 1:
            summary['mean_decode_delivery_fps'] = (int(samples[-1]['frame']) - int(samples[0]['frame'])) / (samples[-1]['wall_elapsed_s'] - samples[0]['wall_elapsed_s'])
        summary['reported_size_note'] = 'Read camera-frame.jpg dimensions; no synthetic source is used.'
    (args.output / 'receiver.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps({key: value for key, value in summary.items() if key != 'samples'}))
    if not wall_timer_reached or not samples or int(samples[-1].get('frame', 0)) < args.seconds or (args.output / 'decode.log').stat().st_size:
        raise SystemExit('Stream verification failed; inspect decode.log and receiver.json.')


if __name__ == '__main__':
    main()
