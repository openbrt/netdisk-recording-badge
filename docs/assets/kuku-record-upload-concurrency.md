**English** · [简体中文](kuku-record-upload-concurrency.zh_CN.md)

# Netdisk Recording Badge concurrent recording and upload

Historical acceptance on 2026-10-02 with application v0.3.9 on the ESP32-C3 badge. The newer continuous recording workflow is described in the user guide.
The implemented behavior is also documented in the [user guide](../kuku-badge-user-guide.md).

## Runtime ownership

- A new recording can start while completed recordings are pending or uploading. Wi-Fi, authorization, and at least 256 KB free storage are still required. Low space saves a shorter segment; the two-minute cap remains.
- Capture runs at priority 12, file writing at 10, and upload at 4. An 8 KiB PCM queue holds approximately 256 ms at 16 kHz, 16-bit mono. Overflow ends the segment with an error instead of silently continuing.
- The writer uses `.REC` while capturing, repairs the WAV header and closes the file, then publishes `.WAV` under the catalog mutex. The uploader sees only published files. On boot, interrupted `.REC` files with valid WAV headers are repaired and published without overwriting an existing `.WAV`.
- Each upload batch owns copies of its filenames. Publishing a new recording or renaming a successfully uploaded file cannot shift the batch's indices. A recording saved during upload requests another batch; one uploader owns the queue at a time.
- Catalog operations hold the catalog mutex. PCM reads and HTTPS operations never hold that mutex. FatFs supplies its own per-volume synchronization for separate file handles.
- During an existing upload, recording startup reuses the current network/authorization state and skips a second TLS probe. A standalone recording still checks both cloud hosts before starting. Upload failures retain closed files for retry and do not cancel capture.

## Memory configuration

Early device iterations reached RSA/TLS allocation failures during concurrency. The final configuration uses dynamic TLS buffers, a 16 KiB incoming TLS record limit, a 2 KiB outgoing limit, 1 KiB upload chunks, four static Wi-Fi RX buffers, bounded dynamic radio buffers, and 2880-byte TCP windows. The existing measured 12 KiB upload stack is retained. The startup worker uses 4 KiB when no network probe is needed. I2S interrupts remain executable during Flash writes.

At the observed concurrent heartbeat, free heap was 22052 bytes and the largest block was 7680 bytes. The reported `min=464` is ESP-IDF's sum of per-region historical low watermarks; it is not a simultaneous free-heap snapshot. This short test does not establish a worst-case allocation margin for every network or filesystem condition.

## Validation and exact firmware

| Check | Result |
| --- | --- |
| Complete repository gate, ESP-IDF 5.5.3 build, image layout and archive verification | PASS |
| Host scheduling, catalog mutation/recovery, capture overflow/failure, and HTTP cleanup/empty-failure-response tests | PASS |
| Device test using production input queue, real microphone/file writes, real HTTPS upload, and USB camera | PASS: 12 assertions |
| Start a second recording while the first upload progresses | PASS: `UP_PCM` progressed from 65536 to 983040 bytes with `rec=1` |
| First segment capture/write counts | PASS: 1036288 / 1036288 bytes, failure=0 |
| Second segment capture/write counts | PASS: 847872 / 847872 bytes, failure=0 |
| Remote multipart MD5 vs local closed-file MD5 | PASS for both segments |
| Final local pending list | PASS: zero files; active `.REC` also stayed outside the pending list |

Full-image SHA-256: `5025c5b09df4610fa4bdcc649ac516ec8196aceb287866d747a7481c1672c493`.

ELF SHA-256: `85a5571c32738033a2315ab1cba9c062a89a67b8490c3d516d59fd1c956067c1`.

Local verified bundle: `build/firmware/5025c5b09df4610fa4bdcc649ac516ec8196aceb287866d747a7481c1672c493/`.
Only the matching application image was flashed at `0x10000`; the compatible partition layout, NVS, and recording storage were preserved.
Local evidence: `/tmp/passport-parallel-e2e-delivery/` (device log, results, pending list, and camera photographs). Raw logs remain local.

## Repeat the scoped device check

The tool resets the selected badge and creates two test segments using its normal automatic cloud upload. It does not erase settings or issue file-deletion commands. It requires pyserial, ffmpeg, the selected USB camera, and a connected/authorized badge.

```bash
python3 tools/test_kuku_parallel_device.py \
  --port /dev/cu.usbmodem212101 \
  --output /tmp/passport-parallel-check \
  --elf-prefix 85a5571c3
```

Unverified: mechanical dual-key ADC detection, calibrated audio quality, two-minute/full-storage/long-running stress, arbitrary RF conditions, and a physical power-cut recovery test. Host recovery and failure injection are reported separately from those device checks.
