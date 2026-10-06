#!/usr/bin/env python3
"""Private LAN telemetry collector and deliberately conditional SOC extrapolation."""
import argparse
import hmac
import json
import math
import os
from pathlib import Path
import statistics
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

EVENTS = {'sample', 'test_armed', 'host_ready', 'usb_host_detached',
          'usb_reconnected', 'recording_started', 'recording_stopped', 'test_off'}
FIELDS = {'boot', 'run', 'seq', 'ms', 'utc', 'event', 'stage', 'host', 'rec', 'wifi',
          'playing', 'cloud', 'soc', 'mv', 'backlight', 'rssi', 'free_kb', 'heap',
          'session', 'segments', 'captured', 'written', 'reason', 'dropped'}
REASONS = ('running', 'manual', 'network', 'storage_space', 'capture', 'storage_io')


def validate_payload(payload):
    if not isinstance(payload, dict) or payload.get('protocol') != 1:
        raise ValueError('unsupported protocol')
    fw = payload.get('firmware', '')
    if not isinstance(fw, str) or len(fw) != 64 or any(c not in '0123456789abcdef' for c in fw):
        raise ValueError('firmware identity required')
    samples = payload.get('samples')
    if not isinstance(samples, list) or not 1 <= len(samples) <= 6:
        raise ValueError('invalid batch size')
    for s in samples:
        if not isinstance(s, dict) or set(s) != FIELDS or s['event'] not in EVENTS:
            raise ValueError('invalid sample fields')
        for field in FIELDS - {'event'}:
            if type(s[field]) is not int:
                raise ValueError('integer sample fields required')
        if any(s[f] not in (0, 1) for f in ('host', 'rec', 'wifi', 'playing')):
            raise ValueError('invalid flags')
        if not (-1 <= s['soc'] <= 100 and -1 <= s['mv'] <= 6000 and
                0 <= s['backlight'] <= 100 and 0 <= s['reason'] < len(REASONS) and
                0 <= s['stage'] <= 3 and 0 <= s['cloud'] <= 3):
            raise ValueError('invalid measurement')
        if any(not 0 <= s[f] < 2**32 for f in
               ('boot', 'run', 'seq', 'session', 'segments', 'heap', 'dropped')):
            raise ValueError('invalid counter')
        if s['seq'] < 1 or s['run'] < 1 or any(not 0 <= s[f] < 2**63 for f in
                                              ('ms', 'captured', 'written')):
            raise ValueError('invalid time/progress')
    return samples


def rate(points):
    """Least-squares percentage points/hour; t is always device monotonic time."""
    if len(points) < 2:
        return 0.0
    start = points[0]['ms']
    xs = [(s['ms'] - start) / 3600000 for s in points]
    ys = [s['soc'] for s in points]
    x, y = statistics.mean(xs), statistics.mean(ys)
    den = sum((v - x)**2 for v in xs)
    return -sum((a - x) * (b - y) for a, b in zip(xs, ys)) / den if den else 0.0


def estimate(points, reserve=10):
    """No result from flat, incomplete, unhealthy or unstable battery observations."""
    result = {'status': 'insufficient_data', 'reserve_soc': reserve,
              'classification': 'SOC extrapolation, not measured battery endpoint',
              'excluded': 'gauge/profile calibration bias and future load changes'}
    if len(points) < 2:
        return dict(result, reason='awaiting_valid_recording_samples')
    elapsed = (points[-1]['ms'] - points[0]['ms']) / 3600000
    decline = points[0]['soc'] - points[-1]['soc']
    result.update(window_minutes=elapsed * 60, soc_start=points[0]['soc'],
                  soc_now=points[-1]['soc'], decline_points=decline, valid_samples=len(points))
    if elapsed < .5:
        return dict(result, reason='need_30_minute_eligible_window')
    expected = elapsed * 360 + 1
    result['coverage'] = min(1, len(points) / expected)
    if result['coverage'] < .9 or any(b['ms'] - a['ms'] > 120000 for a, b in zip(points, points[1:])):
        return dict(result, reason='missing_samples')
    if decline < 10:
        return dict(result, reason='need_10_percentage_point_decline')
    if any(abs(b['soc'] - a['soc']) > 5 or b['soc'] - a['soc'] > 2
           for a, b in zip(points, points[1:])):
        return dict(result, reason='unexplained_soc_step_or_recovery')
    if any(b['captured'] <= a['captured'] or b['written'] <= a['written']
           for a, b in zip(points, points[1:])):
        return dict(result, reason='capture_or_writer_not_progressing')
    if points[-1]['dropped'] != points[0]['dropped']:
        return dict(result, reason='device_queue_dropped_samples')
    r = rate(points)
    midpoint = (points[0]['ms'] + points[-1]['ms']) / 2
    halves = [rate([s for s in points if s['ms'] <= midpoint]),
              rate([s for s in points if s['ms'] >= midpoint])]
    stride = max(1, math.ceil(len(points) / 60))
    subsampled = points[::stride]
    pair_rates = [(a['soc'] - b['soc']) * 3600000 / (b['ms'] - a['ms'])
                  for i, a in enumerate(subsampled) for b in subsampled[i+1:]
                  if b['ms'] - a['ms'] >= 600000]
    robust = statistics.median(pair_rates) if pair_rates else 0
    result.update(rate_points_per_hour=r, half_rates=halves, robust_rate=robust)
    if r <= 0 or any(v <= 0 or abs(v - r) / r > .25 for v in halves + [robust]):
        return dict(result, reason='unstable_soc_trend')
    rates = halves + [r, robust, (decline - 2) / elapsed, (decline + 2) / elapsed]
    lo, hi = min(rates), max(rates)
    if lo <= 0:
        return dict(result, reason='unbounded_sensitivity_range')
    full = max(0, 100 - reserve)
    remaining = max(0, points[-1]['soc'] - reserve)
    return dict(result, status='estimate_available', reason='quality_gates_passed',
                full_hours=full / r, remaining_hours=remaining / r,
                full_hours_range=[full / hi, full / lo],
                remaining_hours_range=[remaining / hi, remaining / lo],
                zero_percent_projection_hours=100 / r,
                uncertainty_type='model sensitivity, not a statistical confidence interval')


class Collector:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        os.chmod(self.directory, 0o700)
        self.lock = threading.RLock()
        self.runs = {}
        self.receipts = {}
        self.offsets = {}
        self.contacts = {}
        self.firmware = {}
        self.history = {}
        self.last_seq = {}
        self.qualified = {}
        self.raw = self.directory / 'samples.jsonl'
        if self.raw.exists():
            for line in self.raw.read_text().splitlines():
                item = json.loads(line)
                s = item['sample']
                key = (s['boot'], s['run'])
                self.runs.setdefault(key, {})[s['seq']] = s
                self.receipts[key] = item['received_at']
                self.firmware[key] = item['firmware']
                offset = item['received_at'] - s['ms']/1000
                self.offsets[s['boot']] = min(self.offsets.get(s['boot'], offset), offset)
        os.chmod(self.raw, 0o600) if self.raw.exists() else None

    def ingest(self, payload, received=None):
        samples = validate_payload(payload)
        received = time.time() if received is None else received
        with self.lock:
            fresh = []
            batch_seen = {}
            for s in samples:
                key = (s['boot'], s['run'])
                batch_key = (*key, s['seq'])
                if batch_key in batch_seen:
                    if batch_seen[batch_key] != s:
                        raise ValueError('batch sequence conflict')
                    continue
                batch_seen[batch_key] = s
                existing = self.runs.get(key, {}).get(s['seq'])
                if existing is not None and existing != s:
                    raise ValueError('sequence conflict')
                if key in self.firmware and self.firmware[key] != payload['firmware']:
                    raise ValueError('firmware changed within run')
                if existing is None:
                    fresh.append((key, s))
            # Durable append before ACK. Duplicate deliveries are idempotent.
            fd = os.open(self.raw, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
            with os.fdopen(fd, 'a') as out:
                for key, s in fresh:
                    out.write(json.dumps({'received_at': received,
                                          'firmware': payload['firmware'], 'sample': s}) + '\n')
                out.flush()
                os.fsync(out.fileno())
            for key, s in fresh:
                self.runs.setdefault(key, {})[s['seq']] = s
                self.receipts[key] = received
                self.firmware[key] = payload['firmware']
            for s in samples:
                key = (s['boot'], s['run'])
                self.contacts[key] = received
                offset = received - s['ms']/1000
                self.offsets[s['boot']] = min(self.offsets.get(s['boot'], offset), offset)
            self.write_report(received)

    def run_report(self, key, current):
        samples = sorted(self.runs[key].values(), key=lambda s: s['seq'])
        latest = samples[-1]
        report = {'boot': key[0], 'run': key[1], 'firmware': self.firmware[key],
                  'last_seq': latest['seq'], 'last_received_at': self.receipts[key],
                  'last_delivery_at': self.contacts.get(key, self.receipts[key]),
                  'telemetry_stale': current - (latest['ms']/1000 + self.offsets[key[0]]) > 180,
                  'latest_soc': latest['soc'], 'latest_mv': latest['mv'],
                  'recording': bool(latest['rec']), 'event': latest['event'],
                  'captured_seconds': latest['captured'] / 32000,
                  'written_seconds': latest['written'] / 32000,
                  'completed_segments': latest['segments'], 'dropped': latest['dropped'],
                  'stop_reason': REASONS[latest['reason']],
                  'audio_verification': 'downloaded WAV and boundary checks pending'}
        detach = next((s for s in samples if s['event'] == 'usb_host_detached'), None)
        if not detach:
            if latest['stage'] == 2 or (latest['stage'] == 3 and latest['event'] != 'test_armed'):
                report.update(status='missing_detach_event', reason='cannot_establish_unplug_time')
                return report
            report['status'] = 'ready_to_unplug' if any(s['event'] == 'host_ready' for s in samples) else 'waiting_for_host'
            if not latest['wifi']:
                report['status'] = 'waiting_for_wifi'
            if report['telemetry_stale']:
                report['status'] = 'telemetry_stale'
            return report
        points, baseline, cause = [], None, None
        start = detach['ms']
        for s in samples:
            if s['ms'] < start:
                continue
            if s['event'] in ('usb_reconnected', 'test_off'):
                cause = s['event']; break
            if not s['rec']:
                if points or s['event'] == 'recording_stopped':
                    cause = REASONS[s['reason']]; break
                continue
            if not s['wifi'] or s['playing'] or s['host'] or s['stage'] != 2 or s['reason']:
                cause = 'workload_or_power_changed'; break
            context = (s['session'], s['backlight'])
            if context != baseline:
                baseline = context; start = s['ms']; points = []
            if s['event'] != 'sample':
                continue
            if s['ms'] < start + 300000 or s['soc'] < 0 or s['mv'] <= 0:
                continue
            if points and s['ms'] <= points[-1]['ms']:
                report.update(status='invalid_data', reason='non_monotonic_device_time')
                return report
            points.append(s)
        fit = estimate(points)
        report['estimate'] = fit
        report['status'] = 'closed' if cause else fit['status']
        if cause:
            report['close_reason'] = cause
        elif not latest['rec']:
            report['status'] = 'waiting_for_recording'
        # Decide stability only on new data, with independent reports 5 min apart.
        hist = self.history.setdefault(key, [])
        if fit['status'] == 'estimate_available' and latest['seq'] != self.last_seq.get(key):
            if not hist or latest['ms'] - hist[-1][0] >= 300000:
                hist.append((latest['ms'], fit['full_hours_range']))
                del hist[:-3]
        if fit['status'] != 'estimate_available':
            hist.clear()
        self.last_seq[key] = latest['seq']
        report['short_test_sufficient'] = (fit['status'] == 'estimate_available' and not cause and not report['telemetry_stale'] and len(hist) == 3 and
            hist[-1][0] - hist[0][0] >= 600000 and
            all((max(h[1][i] for h in hist) - min(h[1][i] for h in hist)) /
                statistics.mean(h[1][i] for h in hist) < .15 for i in (0, 1)))
        if report['short_test_sufficient']:
            self.qualified[key] = dict(fit, qualified_at_ms=latest['ms'])
        if key in self.qualified:
            report['qualified_estimate'] = self.qualified[key]
        if report['telemetry_stale']:
            report['status'] = 'telemetry_stale'
            report['short_test_sufficient'] = False
        return report

    def report(self, current=None):
        current = time.time() if current is None else current
        with self.lock:
            return {'updated_at': current, 'runs': [self.run_report(k, current) for k in self.runs]}

    def write_report(self, current=None):
        report = self.report(current)
        temporary = self.directory / 'report.tmp'
        temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        os.chmod(temporary, 0o600)
        temporary.replace(self.directory / 'report.json')


def make_handler(collector, token):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass  # No tokens or private network paths in access logs.

        def do_GET(self):
            if self.path != '/health':
                self.send_error(404); return
            body = b'{"ok":true,"protocol":1}'
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers(); self.wfile.write(body)

        def do_POST(self):
            if self.path != '/telemetry':
                self.send_error(404); return
            if not hmac.compare_digest(self.headers.get('X-Endurance-Token', ''), token):
                self.send_error(401); return
            try:
                size = int(self.headers.get('Content-Length', '0'))
                if not 0 < size <= 16384:
                    raise ValueError('size')
                self.connection.settimeout(5)
                payload = json.loads(self.rfile.read(size))
                collector.ingest(payload)
            except (ValueError, TypeError, KeyError, TimeoutError):
                self.send_error(400); return
            self.send_response(204)
            self.send_header('Content-Length', '0'); self.end_headers()
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True, help='private JSON: bind, port, token, output')
    args = parser.parse_args()
    config = json.loads(Path(args.config).read_text())
    token = config['token']
    if not isinstance(token, str) or not 16 <= len(token) <= 64 or not token.isascii() or not token.isalnum():
        raise SystemExit('Invalid authentication token')
    collector = Collector(config['output'])
    server = ThreadingHTTPServer((config['bind'], config.get('port', 8765)), make_handler(collector, token))
    server.daemon_threads = True
    def refresh():
        while True:
            with collector.lock:
                collector.write_report()
            time.sleep(30)
    threading.Thread(target=refresh, daemon=True).start()
    print('Private endurance collector ready', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
