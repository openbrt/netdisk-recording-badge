[简体中文](recording-endurance-test-plan.zh_CN.md) · **English**

# Recording endurance and side indicator test plan

## Scope and present status

This plan estimates recording endurance from a short battery-powered observation with automatic device telemetry to a background collector, and investigates a dim recording indicator on the Netdisk Recording Badge. Full-discharge trials and eight-hour reliability runs are optional follow-up checks, not prerequisites for the first estimate. Test instrumentation and the [private collector](../../../tools/endurance/README.md) are implemented; the 2026-10-06 battery-curve results and their limits are recorded separately in [recording endurance](../../recording-endurance.md). Use the [user guide](../../kuku-badge-user-guide.md), [hardware specifications](../../hardware-design/specifications.md), and [firmware layout](firmware-layout.md) for the application and board baseline.

The current application records 16 kHz, 16-bit mono PCM, closes segments every 60 seconds, and uploads completed segments. It dims the backlight to 15% after approximately 15 seconds without input while recording. Recording input handling does not provide a DOWN-long screen-off action. Network disconnection ends capture; reconnection retries uploads but does not resume recording. Sources: [application definitions](../../../main/kuku_app.h), [input and idle handling](../../../main/main.c), [capture/writer](../../../main/kuku_rec.c), and [upload scheduling](../../../main/kuku_baidu.c).

PCM generates 32,000 B/s, 1,920,000 payload bytes per full segment, and 115.2 MB per hour, excluding headers. The raw 5 MiB storage partition represents at most 163.84 seconds; filesystem overhead, existing files, and the writer's safety reserve reduce this. Successful uploads and local reclamation must keep up with capture. Payload throughput must exceed 256 kbit/s over time, with additional allowance for connection setup, protocol traffic, and retries. Delaying uploads for several minutes is unsuitable for the current format and partition.

## 1. Side indicator investigation

Evidence checked on 2026-10-05:

| Evidence | Supported conclusion | Limit |
| --- | --- | --- |
| [Official first-use guide](https://ai-passport.folotoy.cn/guides/getting-started/) describes a green light, on while charging and off when full | Its documented function is charging indication | It does not specify electrical wiring or PWM control |
| Local [pin definitions](../../../components/bsp/include/bsp_pins.h) and BSP | Display backlight has a defined control interface; a separate side LED does not | Absence from software does not prove absence of a physical connection |
| Official XiaoZhi [board configuration](https://github.com/FoloToy/folo-ai-passport-xiaozhi/blob/72da544d1b51678f5d88967adc299b3aec956946/main/boards/folotoy/ai-passport/config.h) and [board implementation](https://github.com/FoloToy/folo-ai-passport-xiaozhi/blob/72da544d1b51678f5d88967adc299b3aec956946/main/boards/folotoy/ai-passport/ai_passport_board.cc) | This board does not configure a status LED or override `GetLed()` | It is another firmware's implementation, not a schematic |
| Its [base board](https://github.com/FoloToy/folo-ai-passport-xiaozhi/blob/72da544d1b51678f5d88967adc299b3aec956946/main/boards/common/board.cc#L56) returns `NoLed`; the intermediate `WifiBoard` does not override it | That official firmware has no implemented side-LED control path | Electrical controllability remains unverified |

Working conclusion: a charging-circuit indicator is plausible, but software control is not established. Do not assign or toggle an undocumented GPIO to search for the LED.

Investigate in this order:

1. Identify the actual lamp, board revision, position, and color. Printed status legends or product renderings do not establish wiring.
2. Observe it during USB charging with the application on, hardware power off while charging, full charge, and battery-only operation. Record observations externally. A lamp that works with the application off supports autonomous hardware control; it does not exclude a shared control connection.
3. Obtain the matching schematic or manufacturer pin definition. If unavailable, a separate physical inspection can trace the LED and resistor to the charger status output, MCU, or another driver. Continuity tracing is performed with USB and battery disconnected; opening the enclosure and probing are separate physical work, not performed by this plan.
4. Record the controller/pin, polarity, resistor/current path, shared connections, and whether brightness control is supported. A MCU-connected lamp still needs a verified idle level and suitable control circuit.
5. If controllable, add the confirmed interface to the BSP and test off/low brightness/brief pulses without disturbing charging, audio, or existing pin owners. If charger-only, a separate recording LED would require a hardware change. Screen backlight pulses are a separate alternative, not evidence of side-LED control.

Proposed indication, pending hardware confirmation: lowest reliably visible brightness, approximately 100 ms on every 3 seconds while recording. Gate pulses on recording state and recent progress of PCM capture/writing, not merely a running task flag. Stop indication after recording stops or a fault is detected. Verify pulse timing, visibility, current cost, and no interference with capture; use a non-blocking worker/timer path. Do not infer average board savings from LED duty cycle alone.

## 2. Test matrix

| ID | Configuration | Purpose | Execution status |
| --- | --- | --- | --- |
| B1 | Functional firmware plus test telemetry; fully charged, computer USB data cable removed, normal 15% recording backlight; initially observe 30–60 minutes | Estimate current recording endurance from SOC decline while verifying capture/upload | Implemented; physical acceptance required |
| B2 | B1 with recording automatic screen-off | Compare the estimated benefit under matched conditions | Requires screen-off implementation and acceptance |
| B3 | B2 plus confirmed side-LED pulses | Compare estimated indicator cost and verify indication | Requires confirmed hardware and firmware |
| B4 | B2 plus one audio or CPU power optimization per variant | Compare consumption trends and reliability | Requires separate implementation and regression checks |
| U1 | USB-powered continuous capture for 8 hours, optionally 24 hours | Validate long-run capture/upload reliability | Optional follow-up; short B1 does not prove it |
| D1 | Battery-powered recording to a supported power-loss endpoint | Calibrate the estimate against an actual runtime | Optional follow-up, not required for this short test |

The test firmware includes volatile USB arming, detach reporting, and a private background collector. B2/B3/B4 are also future modes. Keep the same telemetry overhead, Wi-Fi connection, and 60-second segment rotation for comparisons. Deep sleep ends recording. Evaluate CPU frequency scaling separately; [ESP-IDF I2S power management](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/peripherals/i2s.html#power-management) requires clock/lock handling before Light-sleep can be considered.

## 3. Common preparation and automatic start

1. Bind the physical badge to an exact firmware image/ELF identity, partition layout, and source/configuration. Use a functional cloud configuration, not public gate placeholder credentials. Any update follows the compatible segmented-flash policy and preserves NVS and recordings.
2. Complete Wi-Fi/cloud authorization and clock synchronization. Let pending uploads finish and check space for more than a full segment plus the writer reserve. Never delete unsynced recordings. Fix AP/location, network, ambient conditions, photo/assets, and audio workload between modes.
3. Start a private background collector reachable from the badge over Wi-Fi. Configure the test endpoint and per-device authentication privately; do not publish endpoint secrets. The collector stores telemetry and produces a rolling report automatically. USB serial is only for setup; wireless reporting continues after removal.
4. Charge normally until the previously lit charging indicator goes out. Record starting SOC/voltage and charge history; SOC alone does not prove full charge. Use the same settling procedure between runs. Play a fixed, non-private marker track with an independent clock for audio verification.
5. Arm the test explicitly while connected to an awake computer through a data-capable USB cable. After stable host detection for 10 seconds, a connected-to-disconnected transition lasting 5 seconds emits `usb_host_detached` with a monotonic device timestamp, boot/session IDs, SOC/voltage, and recording state. These durations are proposed debounce settings, to be verified on the board. Send immediately over Wi-Fi; the collector acknowledges and starts the observation automatically. Do not wait for USB serial after unplugging.
6. If recording already runs, begin measurement without restarting it. Otherwise report `waiting_for_recording`; the normal physical recording gesture starts capture, and a `recording_started` event establishes the eligible recording interval. Unplugging alone does not start microphone capture. Do not use `REC 28800`: the current diagnostic command clamps to 600 seconds; use the normal application recording path.

[ESP-IDF 5.5.3 host detection](https://github.com/espressif/esp-idf/blob/v5.5.3/components/esp_driver_usb_serial_jtag/include/driver/usb_serial_jtag.h) uses incoming USB SOF packets. It detects a data host, not VBUS power or charging: wall chargers/power banks are not detected as connected, and host suspend/data interruption may appear as detach while power remains. The first test therefore requires an awake computer and complete physical cable removal; record this operating condition. Before accepting data, verify plug/unplug and serial-port-close behavior, and test host sleep/data loss as ambiguous cases. Reconnection interrupts the estimate. Booting without USB does not fabricate a detach event. Generic charger removal needs a confirmed VBUS/charger-status interface; no such board pin is currently defined. Do not infer unplugging from a voltage drop or assign an undocumented GPIO.

## 4. Wireless telemetry and background process

The implemented test instrumentation uses `ENDURANCE ARM <url> <token>`, `ENDURANCE STATUS`, and `ENDURANCE OFF`; setup and protocol details are in the [collector guide](../../../tools/endurance/README.md). Read [battery SOC/voltage](../../../components/bsp/src/bsp_battery.c) every 10 seconds through a worker using the existing shared I2C ownership. Do not wake the screen or change brightness. The current SOC API returns integer percentage points; although it reads two register bytes, it discards the fractional byte. More samples do not turn it into calibrated high-resolution capacity data.

Batch six samples into an authenticated Wi-Fi request about once per minute, preferably reusing a connection. Immediate events cover detach, recording start/stop, reconnect, and faults. Network/I2C work must not block capture, button callbacks, or LVGL. A 32-entry RAM queue plus six pending samples, sequence IDs, acknowledgements, idempotent retries, and capped backoff prevents duplicate events or unlimited backlog. Record dropped samples and delayed deliveries; telemetry must yield to audio uploads and must not change their existing Wi-Fi power policy. Measure added overhead consistently between modes; the estimate describes the instrumented workload until that overhead is quantified.

| Fields | Use |
| --- | --- |
| Test/device pseudonym, boot ID, session ID, mode, firmware identity, sample sequence | Separate runs, resets, retries, and comparable variants |
| Monotonic sample timestamp, optional UTC, collector receipt time | Fit device sample time; receipt delay is not battery observation time |
| Host connected, detach source, test armed, power-source confidence | Distinguish the controlled unplug test from unknown charging state |
| SOC %, battery mV, read-valid flag | SOC trend; failed reads are invalid, never 0% |
| Recording state, 64-bit PCM/sample progress, segment sequence, stop reason | Confirm actual capture progress; do not rely only on a task flag or wrapping byte counter |
| Actual backlight level/mode, Wi-Fi state/RSSI, upload backlog, free space, heap/reset information where available | Reject changed workload and classify failures |

The collector appends private JSONL/CSV, deduplicates sequences, and updates a report every minute. After three missing report intervals, mark telemetry stale and freeze the last estimate; missing messages do not prove battery exhaustion. Segment arrival is supporting evidence only, as upload lag is variable. A reset creates a new boot interval. Stop, reconnect, mode change, or capture/upload failure closes the eligible interval with its cause. Telemetry outages cannot force recorder stop; the current independent Wi-Fi-loss stop behavior remains visible. Retrying a buffered detach event preserves its original timestamp and does not restart the collector's clock.

Before physical use, host checks should cover arm/debounce/reconnect/reset state transitions, invalid SOC, quantized/flat/noisy traces, missing/delayed/duplicate reports, and recording faults. Device checks must verify actual cable removal, continuing Wi-Fi reporting, unchanged screen behavior, and no capture disruption. Collector deployment is private and test-specific; maintained documents contain no endpoint or token.

## 5. B1: short observation and runtime estimation

1. After the automatic detach/start report, leave the badge recording without screen interaction. The background process gathers data for an initial 30–60 minutes. Exclude at least the first 5 minutes after detach and recording start to reduce charging/load transients; extend this exclusion if the readings still recover or rise.
2. Use a contiguous interval of at least 30 minutes of healthy, unchanged battery recording after the excluded period. Require at least a 10 percentage-point decline, at least 90% of expected valid samples, and no sampling gap over two minutes. A full-charge SOC plateau is insufficient evidence; wait for a usable decline. These are initial engineering quality gates, not guaranteed gauge accuracy.
3. Fit `SOC(t) = a - r × t` to valid samples, with time in hours and `r > 0` in percentage points/hour. Compare slopes from the two interval halves and a robust fit against the ordinary fit; differences above 25%, persistent rises, or an abrupt unexplained SOC step produce an unstable/data-insufficient result. Retain rejected points and reasons. Never fit across reconnects, resets, mode changes, recording stops, or network/storage failures.
4. For a declared reporting reserve `S_reserve = 10%`, calculate `T_remaining = max(0, S_now - S_reserve) / r` and the full-charge equivalent `T_full = (100 - S_reserve) / r`. The 10% reserve is an estimate/reporting convention, not an implemented cutoff or measured board shutdown threshold. An optional projection to 0% is shown separately. Do not add the unobserved initial plateau as measured recording time or use voltage decline alone as linear capacity loss.
5. Example only: a usable 40-minute interval declines by 12 points, giving `r = 18 points/hour`; the estimated full-charge time to the declared 10% reserve is 5 hours, or about 5.6 hours to 0%. These are illustrative arithmetic, not badge results. Mode comparisons use the same reserve and eligible SOC range.
6. Report the fit window, SOC range, rate, actual verified audio duration, latest SOC, estimated remaining/full-charge time, and workload. For a sensitivity band, compare half-window/robust rates and endpoint rates with a conservative ±2-point change allowance for integer quantization; positive rates give a min/max runtime range. If the allowed rate reaches zero, withhold a finite upper estimate. This is model sensitivity, not a statistical confidence interval; it excludes uncalibrated gauge/profile bias and future load/low-battery changes. SOC calibration remains unverified until separate battery validation.
7. Finish once quality gates are met and the projected range changes by less than 15% over three reports spanning at least 10 minutes. If not met at 60 minutes, the collector reports the missing condition; continue only as useful for a short observation, or end with insufficient data. Full discharge is not mandatory. Stop normally, let the last WAV upload, and retain faults instead of silently restarting. An early stop is a reliability finding, not a battery-runtime estimate.

A short SOC slope estimates runtime under the observed load. It does not prove continuous operation for the projected hours, a calibrated capacity, or the actual power-off time. Optional U1/D1 can establish those separately. For mode comparison, use matched short intervals and repeat only when the model spread or changed conditions prevent a useful comparison.

## 6. WAV and boundary validation

After each trial, list the relevant date folder and download all trial segments through the authenticated tool. Use the actual folder and filenames; trials spanning midnight cover both folders. For example:

```text
tools/bdverify/bdverify ls YYYY-MM-DD
tools/bdverify/bdverify get YYYY-MM-DD/RECORDING.WAV
tools/bdverify/bdverify md5 YYYY-MM-DD/RECORDING.WAV
```

Deduplicate retries using the intended remote path and file content. Check every downloaded WAV: RIFF/WAVE header, sample rate, channels, bit depth, actual length versus declared length, even payload length, and decoded sample count. Full segments should contain 960,000 mono samples; the final segment may be shorter. Sum unique decoded samples divided by 16,000 to obtain `T_audio`, the verified recorded duration. For a normal short test, record the final completed/uploaded segment. If an optional power-loss test is performed, distinguish pre-shutdown uploads from audio recovered after power restoration.

An independently computed downloaded MD5 identifies the returned bytes. It proves agreement with local content only when an independently captured local digest or byte reference exists; a cloud metadata MD5 field alone is not sufficient. Current automatic upload cleanup can remove successful local files, so full local/cloud digest comparison requires separate instrumentation or an available retained local reference.

Align the captured continuous marker track with the external reference, accounting for the two clocks' gradual drift. Investigate any proven missing/repeated audio; clock drift itself is not a segment gap. Listen to the concatenated audio and inspect at least two seconds around every segment boundary. Valid headers, correct file counts, or nonzero amplitude alone do not prove uninterrupted microphone capture.

## 7. Optional battery-current and indicator measurement

For optimization diagnosis, use a suitable power analyzer on the verified battery supply path with all device supply routes accounted for. This requires a separately confirmed physical setup; connecting through the battery path or opening the case is not performed by the documentation task. Avoid instrument burden or another supply path changing device behavior.

Measure comparable states: battery idle with screen off, recording with 15% backlight, recording with zero backlight after B2 exists, upload burst, and B3 indicator pulses. Record battery voltage, average current, sampling rate/bandwidth, and temperature. A low-rate meter must not be reported as measuring true Wi-Fi current peaks. Integrate current for consumed charge, and voltage times current for energy. Board average current is the relevant denominator for capacity estimates; chip datasheet sleep current is not whole-badge recording current.

For B3 measure the extra charge/energy per pulse period against B2. Verify the lamp stops on manual stop, network-induced stop, storage stop, and capture failure; it must not continue presenting a healthy-recording heartbeat after PCM progress ends. Check low battery visibility and charging indication coexistence. Quantify the endurance difference only from comparable trials.

## 8. Results and acceptance

The background report contains test/firmware/mode identity, detach event and power-source assumptions, initial SOC/voltage, eligible sample interval, valid/missing counts, SOC decline/rate, reserve convention, estimated remaining/full-charge runtime and sensitivity range, `T_audio`, WAV/boundary findings, upload/backlog state, stop/fault reasons, and observation overhead. Keep raw telemetry/audio private.

| Question | Acceptance/reporting rule |
| --- | --- |
| Does unplugging automatically start collection? | Armed, verified computer-cable detach produces one acknowledged event; wireless samples continue, screen stays unchanged, and capture is not disrupted. Host sleep/data-only loss is identified as an ambiguity, not verified VBUS removal |
| Can a short observation estimate runtime? | The quality/stability gates in section 5 pass; report an estimate and sensitivity range with reserve and assumptions. Flat/invalid/unstable readings give insufficient data |
| Is observed recording healthy? | Samples and downloaded WAV progress throughout the actual interval, no demonstrated gaps/repetition/corruption, unrequested stop/reset, or growing unresolved backlog |
| Is screen-off better? | Compare B2/B1 rates and projected ranges under matched conditions; overlapping uncertain ranges do not establish a winner |
| Is the recording light usable? | Confirm wiring first; B3 tracks recent healthy capture and stops on faults, with comparable telemetry/current cost |
| Has actual full-charge duration been measured? | Only optional D1 reaching an evidenced battery endpoint establishes it; B1 remains an extrapolation |

First execution order: implement and verify the armed detach event, wireless telemetry, and collector; conduct one B1 short run and produce the estimate if data qualify; compare implemented B2/B3 variants as useful. LED wiring investigation can proceed independently. Neither eight-hour U1 nor three full-discharge trials is a prerequisite. Instrumentation is implemented and host-tested; physical unplug/reporting acceptance is tracked separately. A qualified battery-curve estimate is now available; see [recording endurance](../../recording-endurance.md). A direct full-charge runtime and side-LED electrical control remain unverified until the corresponding trials.
