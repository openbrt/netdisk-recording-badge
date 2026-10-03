"""Record two real segments and prove closed-file upload overlaps capture.

Uses production input events; does not test mechanical button/ADC detection.
Keeps normal automatic cloud upload enabled and never erases settings/files.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time
import serial


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--elf-prefix', required=True)
    parser.add_argument('--camera-index', default='0')
    parser.add_argument('--seed-seconds', type=int, default=30)
    args = parser.parse_args()
    root = Path(args.output)
    root.mkdir(parents=True, exist_ok=True)
    results = []
    transcript = ''
    port = serial.Serial(args.port, 115200, timeout=0.1)
    with (root / 'device.log').open('w') as log:
        def read(seconds):
            nonlocal transcript
            end = time.monotonic() + seconds
            data = bytearray()
            while time.monotonic() < end:
                data.extend(port.read(port.in_waiting or 1))
            text = data.decode('utf-8', errors='replace')
            transcript += text
            log.write(text)
            log.flush()
            assert not any(item in text for item in
                           ('Guru Meditation', 'CORRUPT HEAP', 'assert failed',
                            'Stack canary', 'PCM queue overrun'))
            for line in text.splitlines():
                if any(item in line for item in ('STATE:', 'PCM:', 'UP_PCM:', '本轮', '传输失败')):
                    print(line, flush=True)
            return text

        def cmd(command, seconds=0.2):
            log.write('\n>>> ' + command + '\n')
            port.write((command + '\n').encode())
            return read(seconds)

        def check(name, passed):
            results.append({'test': name, 'pass': bool(passed)})
            (root / 'results.json').write_text(json.dumps(results, indent=2))
            print(name, 'PASS' if passed else 'FAIL', flush=True)
            assert passed, name

        def state():
            text = cmd('STATE')
            pattern = r'STATE: page=(\d+) rec=(\d+) play=(\d+) off=(\d+) wifi=(\d+) bd=(\d+) bytes=(\d+) ms=(\d+)'
            match = re.search(pattern, text)
            for _ in range(30):
                if match:
                    break
                text += read(0.1)
                match = re.search(pattern, text)
            assert match, 'Missing STATE response'
            return dict(zip(('page', 'rec', 'play', 'off', 'wifi', 'bd', 'bytes', 'ms'),
                            map(int, match.groups())))

        def key(button, long=False):
            text = cmd('KEY ' + button + (' LONG' if long else ' CLICK'), 0.05)
            for _ in range(40):
                if 'KEY: queued rc=0' in text:
                    return text
                text += read(0.1)
            raise AssertionError('Missing KEY response')

        def photo(name):
            subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'avfoundation',
                            '-pixel_format', 'uyvy422', '-framerate', '30', '-i', args.camera_index + ':none',
                            '-frames:v', '1', '-y', str(root / (name + '.jpg'))],
                           check=True, capture_output=True, timeout=15)

        def start():
            key('CHORD')
            for _ in range(15):
                current = state()
                if current['rec']:
                    return current
                read(0.5)
            raise AssertionError('Recording never started')

        try:
            # Explicit test reset binds the observed runtime to this exact ELF.
            port.dtr = False
            port.rts = True
            time.sleep(0.1)
            port.rts = False
            time.sleep(0.05)
            port.dtr = True
            boot = read(12)
            check('matching firmware', args.elf_prefix in boot and 'v0.3.9' in boot)
            initial = state()
            check('network and authorization retained', initial['wifi'] == 1 and initial['bd'] in (2, 3))
            check('first segment starts', start()['rec'] == 1)
            read(args.seed_seconds)
            first = state()
            check('first PCM grows', first['bytes'] >= args.seed_seconds * 24000)
            key('OK', True)
            stopped = state()
            for _ in range(15):
                if stopped['rec'] == 0 and stopped['bd'] == 3:
                    break
                read(0.05)
                stopped = state()
            check('first segment closes and uploads', stopped['rec'] == 0 and stopped['bd'] == 3)
            overlap_start = len(transcript)
            second = start()
            check('second recording starts during upload', second['rec'] == 1 and second['bd'] == 3)
            photo('parallel')
            for _ in range(4):
                read(5)
                state()
            later = state()
            check('second PCM grows during upload', later['rec'] == 1 and later['bytes'] > second['bytes'] + 320000)
            check('actual upload bytes advance during capture',
                  bool(re.search(r'UP_PCM: sent=\d+ rec=1', transcript[overlap_start:])))
            local = cmd('LS')
            (root / 'pending-during-recording.txt').write_text(local)
            photo('recording-after-upload')
            key('OK', True)
            stopped = state()
            for _ in range(15):
                if stopped['rec'] == 0:
                    break
                read(0.05)
                stopped = state()
            check('second segment stops', stopped['rec'] == 0)
            photo('saved')
            for _ in range(12):
                local = cmd('LS')
                if 'LS: 0 file(s)' in local:
                    break
                read(5)
            check('both recordings finish upload', 'LS: 0 file(s)' in local)
            pcm = re.findall(r'PCM: captured=(\d+) written=(\d+) failure=(\d+)', transcript)
            check('capture equals written without overflow',
                  len(pcm) >= 2 and all(int(a) > 0 and a == b and failure == '0'
                                       for a, b, failure in pcm[-2:]))
            check('remote multipart MD5 verified', len(re.findall(r'PART: .* md5=[0-9a-f]{32}', transcript)) >= 2)
            cmd('BAIDU LIST', 2)
            read(5)
            cmd('BAIDU LIST STATUS')
        finally:
            # End a test segment even when a test assertion fails.
            try:
                if state()['rec']:
                    key('OK', True)
            finally:
                port.close()
    print('Evidence:', root, flush=True)


if __name__ == '__main__':
    main()
