[简体中文](README.zh_CN.md) · **English**

# Private recording endurance collector

The Python standard-library collector receives authenticated telemetry from the badge over a trusted LAN. It does not receive audio or connect to Baidu. It appends `samples.jsonl` before acknowledging each batch and writes `report.json` every 30 seconds and after new samples. Keep configuration, outputs, and tokens private, for example under `.local/endurance/`.

Create a private JSON configuration with `bind` (the computer LAN IPv4 address), `port` (for example 8765), `token` (a random 16–64-character ASCII alphanumeric secret), and `output` (a private directory). Use file permissions 0600. Start:

```text
python3 tools/endurance/collector.py --config <private-config.json>
```

Keep the computer awake and the process running. This HTTP listener is for a trusted private LAN only; do not expose it to the Internet. HTTP carries the token without encryption. HTTPS badge endpoints validate through the ESP-IDF certificate bundle, but this small collector does not provision TLS.

After confirming the badge's Wi-Fi can reach the computer, send over USB:

```text
ENDURANCE ARM http://<computer-lan-ip>:8765/telemetry <private-token>
ENDURANCE STATUS
```

Configuration is volatile. ARM returns 0 when queued, -1 for invalid input/unavailable instrumentation, -2 when busy, -3 if an address/token change requires reboot, and -4 after a permanent collector rejection requiring corrected configuration and reboot. Ten seconds of stable data-host presence creates `host_ready`; the report shows `ready_to_unplug`. Confirm recent acknowledged samples, valid SOC/voltage, Wi-Fi/cloud readiness, and recording progress before unplugging. When host absence lasts five seconds, the collector receives `usb_host_detached`. Unplugging does not start audio. The diagnostic `REC` command stops after at most ten minutes, so use normal recording buttons. `ENDURANCE OFF` stops collection without stopping audio. Re-arm after OFF to create another run using the same endpoint/token; reboot for different configuration.

The sender holds six pending samples plus a 32-entry queue, retries with capped backoff, and reports queue drops. Permanent HTTP 4xx rejection (except 408/429) disables telemetry without stopping audio; STATUS reports `http_error`, and corrected configuration requires reboot. Sampling runs independently of HTTP and does not change the backlight. Fields include boot/run/sequence, monotonic time, firmware ELF identity, SOC/voltage, host/recording/network state, commanded brightness, 64-bit PCM byte progress, closed segment count, storage headroom, heap, and recorder stop reason. `stage` is 0 waiting for a host, 1 ready, 2 detached, or 3 closed; recorder reasons are 0 running, 1 normal/manual stop, 2 network, 3 space, 4 capture, or 5 storage I/O. Data-host loss alone cannot prove VBUS loss: use an awake computer and physically remove the whole cable.

The report excludes five minutes after detach/recording/brightness transitions, requires at least 30 eligible minutes, ten SOC points of decline, adequate sample coverage, PCM progress, and consistent regression/robust/half-window slopes. It reports remaining/full-charge time to a declared 10% reserve plus a separate 0% projection. The range is model sensitivity, not a calibrated confidence interval. Three consistent reports separated over ten minutes mark `short_test_sufficient`; the process does not stop recording automatically. Stale telemetry freezes the result and cannot establish battery exhaustion. Validate downloaded WAVs separately. See the [full test plan](../../docs/development/engineering/recording-endurance-test-plan.md).

Tests are part of `./tools/validate.sh --static`; run the collector cases alone with `python3 tests/test_endurance_collector.py`. They validate protocol and synthetic curves, not physical battery calibration or USB behavior.

Telemetry HTTP requests wait while a Netdisk upload batch owns the network memory; sampling continues and preserves event timestamps. Sender/sampler stacks are 4096/3072 bytes. USB `STATS` provides PCM counters, firmware identity, heap and all available task stack watermarks. Commands are assembled to a newline across USB packets; overlong or NUL-containing lines are rejected in full.
