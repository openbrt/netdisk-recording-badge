"""Protocol, real HTTP ACK, and representative SOC traces (not board calibration)."""
import importlib.util
import json
from pathlib import Path
import tempfile
import threading
import unittest
from urllib.request import Request, urlopen
from urllib.error import HTTPError
from http.server import ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('endurance_collector', ROOT / 'tools/endurance/collector.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def sample(index, event='sample', **overrides):
    result = dict(boot=1, run=2, seq=index+1, ms=index*10000, utc=1700000000+index*10,
                  event=event, stage=2, host=0, rec=1, wifi=1, playing=0, cloud=2,
                  soc=90-int(index/15), mv=4000, backlight=15, rssi=-50, free_kb=3000,
                  heap=70000, session=1, segments=index//6, captured=index*320000,
                  written=index*320000, reason=0, dropped=0)
    result.update(overrides)
    return result


def payload(samples):
    return dict(protocol=1, firmware='a'*64, samples=samples)


class EstimateTest(unittest.TestCase):
    def test_quantized_trace_and_model_range(self):
        points = [sample(i) for i in range(241)]
        report = module.estimate(points)
        self.assertEqual(report['status'], 'estimate_available')
        self.assertAlmostEqual(report['full_hours'], 3.75, delta=.04)
        self.assertLess(report['full_hours_range'][0], report['full_hours'])
        self.assertGreater(report['full_hours_range'][1], report['full_hours'])
        self.assertEqual(report['classification'], 'SOC extrapolation, not measured battery endpoint')

    def test_short_flat_invalid_sparse_and_unhealthy(self):
        for points, reason in (
            ([sample(i) for i in range(30)], 'need_30_minute_eligible_window'),
            ([sample(i, soc=100) for i in range(241)], 'need_10_percentage_point_decline'),
            ([sample(i) for i in range(0, 241, 2)], 'missing_samples'),
            ([sample(i, captured=1) for i in range(241)], 'capture_or_writer_not_progressing'),
            ([sample(i, dropped=int(i>100)) for i in range(241)], 'device_queue_dropped_samples')):
            with self.subTest(reason=reason):
                self.assertEqual(module.estimate(points)['reason'], reason)

    def test_soc_jump_and_changed_consumption(self):
        points = [sample(i) for i in range(241)]
        points[50]['soc'] += 10
        self.assertEqual(module.estimate(points)['reason'], 'unexplained_soc_step_or_recovery')
        points = [sample(i, soc=90-int(i/20) if i<120 else 84-int((i-120)/6)) for i in range(241)]
        self.assertEqual(module.estimate(points)['reason'], 'unstable_soc_trend')


class CollectorTest(unittest.TestCase):
    def test_detach_session_settling_staleness_and_reconnect(self):
        with tempfile.TemporaryDirectory() as d:
            c=module.Collector(d)
            c.ingest(payload([sample(0,'test_armed',stage=0,host=1)]),received=100)
            c.ingest(payload([sample(1,'host_ready',stage=1,host=1)]),received=101)
            self.assertEqual(c.report(102)['runs'][0]['status'],'ready_to_unplug')
            self.assertEqual(c.report(400)['runs'][0]['status'],'telemetry_stale')
            c.ingest(payload([sample(2,'usb_host_detached')]),received=102)
            for i in range(3,333):
                c.ingest(payload([sample(i)]),received=100+i)
            r=c.report(435)['runs'][0]
            self.assertEqual(r['estimate']['status'],'estimate_available')
            self.assertTrue(r['short_test_sufficient'])
            self.assertGreaterEqual(r['estimate']['window_minutes'],30)
            self.assertEqual(c.report(1000)['runs'][0]['status'],'telemetry_stale')
            self.assertFalse(c.report(1000)['runs'][0]['short_test_sufficient'])
            c.ingest(payload([sample(333,'usb_reconnected',host=1,stage=3)]),received=440)
            self.assertEqual(c.report(441)['runs'][0]['close_reason'],'usb_reconnected')
            self.assertFalse(c.report(441)['runs'][0]['short_test_sufficient'])

    def test_duplicate_delay_conflict_and_restart(self):
        with tempfile.TemporaryDirectory() as d:
            c=module.Collector(d)
            s=sample(10,'usb_host_detached')
            c.ingest(payload([s]),received=100)
            c.ingest(payload([s]),received=500)
            self.assertEqual(len(c.runs[(1,2)]),1)
            self.assertTrue(c.report(500)['runs'][0]['telemetry_stale'])
            with self.assertRaises(ValueError):
                c.ingest(payload([dict(s,soc=42)]))
            with self.assertRaises(ValueError):
                c.ingest(payload([sample(11),sample(11,soc=42)]))
            reloaded=module.Collector(d)
            self.assertEqual(reloaded.runs,c.runs)
            self.assertEqual(len(Path(d,'samples.jsonl').read_text().splitlines()),1)

    def test_stop_and_invalid_read_not_zero(self):
        with tempfile.TemporaryDirectory() as d:
            c=module.Collector(d)
            c.ingest(payload([sample(0,'usb_host_detached',rec=0)]),100)
            self.assertEqual(c.report(100)['runs'][0]['status'],'waiting_for_recording')
            c.ingest(payload([sample(1,'recording_started'),sample(2,soc=-1,mv=-1)]),101)
            self.assertEqual(c.report(101)['runs'][0]['estimate']['reason'],'awaiting_valid_recording_samples')
            c.ingest(payload([sample(3,'recording_stopped',rec=0,reason=2)]),102)
            self.assertEqual(c.report(102)['runs'][0]['close_reason'],'network')

    def test_missing_detach_and_delayed_sample_are_not_ready(self):
        with tempfile.TemporaryDirectory() as d:
            c=module.Collector(d)
            c.ingest(payload([sample(0,'host_ready',stage=1,host=1)]),100)
            c.ingest(payload([sample(1,stage=2)]),101)
            self.assertEqual(c.report(101)['runs'][0]['status'],'missing_detach_event')
            c.ingest(payload([sample(2,'usb_host_detached')]),500)
            self.assertTrue(c.report(500)['runs'][0]['telemetry_stale'])
            self.assertEqual(c.report(500)['runs'][0]['status'],'telemetry_stale')

    def test_same_time_events_do_not_fake_capture_stall(self):
        with tempfile.TemporaryDirectory() as d:
            c=module.Collector(d)
            c.ingest(payload([sample(0,'usb_host_detached')]),100)
            for i in range(1,281):
                s=sample(i,seq=i*2+1)
                batch=[s]
                if i==100:
                    batch.append(sample(i,'recording_started',seq=i*2+2))
                c.ingest(payload(batch),received=100+i*10)
            self.assertEqual(c.report(2900)['runs'][0]['estimate']['status'],'estimate_available')

    def test_http_auth_batch_limit_and_durable_ack(self):
        with tempfile.TemporaryDirectory() as d:
            c=module.Collector(d)
            token='testtoken0123456789'
            server=ThreadingHTTPServer(('127.0.0.1',0),module.make_handler(c,token))
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            url='http://127.0.0.1:'+str(server.server_address[1])+'/telemetry'
            try:
                def post(p,t=token):
                    return urlopen(Request(url,json.dumps(p).encode(),
                                           {'X-Endurance-Token':t,'Content-Type':'application/json'}))
                with self.assertRaises(HTTPError) as e:
                    post(payload([sample(0)]),'wrong')
                self.assertEqual(e.exception.code,401)
                with self.assertRaises(HTTPError) as e:
                    post(payload([sample(i) for i in range(7)]))
                self.assertEqual(e.exception.code,400)
                with post(payload([sample(0)])) as response:
                    self.assertEqual(response.status,204)
                self.assertTrue(Path(d,'samples.jsonl').exists())
                self.assertEqual(len(c.runs[(1,2)]),1)
                with post(payload([sample(0)])) as response:
                    self.assertEqual(response.status,204)
                self.assertEqual(len(c.runs[(1,2)]),1)
            finally:
                server.shutdown(); server.server_close(); thread.join()


if __name__=='__main__':
    unittest.main()
