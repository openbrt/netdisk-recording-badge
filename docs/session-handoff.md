[简体中文](session-handoff.zh_CN.md) · **English**

# Netdisk Recording Badge session handoff

This repository is the standalone recording application extracted from the maintained badge project. Continue development here. Its `main` branch contains the application, not a clean hardware-test template. Demo files retained for host regression tests are not included in `main/CMakeLists.txt`'s firmware sources. No access to the original private repository is required to build the public source.

## Start a new session

1. Read root [AGENTS.md](../AGENTS.md), run `git status --short --branch`, and preserve existing changes.
2. Read this handoff and the [build guide](build.md). On the owner's machine, `.local/session-handoff.zh_CN.md` contains local paths, approved release artifacts, and acceptance records; it is ignored by Git.
3. Use the [user guide](kuku-badge-user-guide.md) and implemented code to establish current behavior. Update both language versions and the on-device guide when controls or features change.
4. Read only the relevant engineering documents and use the matching installed Passport skill. Run the complete gate before delivery and report build, host tests, device tests, and unverified checks separately.

## Current application and source map

The badge records WAV audio, saves completed segments every minute, and uploads them to Baidu Netdisk in the background. It supports account authorization, cloud file browsing, local playback, JPG preview, and a saved badge photo. Network loss saves and stops recording; a restored connection allows pending uploads, while recording resumes through the user's button action.

| Area | Entry points |
| --- | --- |
| Boot, storage, shared application state | `main/main.c`, `main/kuku_app.h` |
| Pages, buttons, status and on-device guide | `main/kuku_ui.c` |
| Capture, segmentation and WAV integrity | `main/kuku_rec.c`, `main/kuku_wav.c`, `main/kuku_rec_filename.c` |
| Netdisk authorization, transfer and paths | `main/kuku_baidu.c`, `main/kuku_baidu_auth_link.c`, `main/kuku_cloud_path.c` |
| Wi-Fi and USB console commands | `main/kuku_wifi.cc`, `main/kuku_test.c`, `components/esp-wifi-connect/` |
| Photos and JPG handling | `main/kuku_image.c`, `assets/fonts/` |
| Hardware drivers | `components/bsp/` |
| Browser installer and USB Wi-Fi setup | `site/`, `tests/test_web_serial.mjs`, `tools/prepare_site.py` |
| Configuration and gates | `partitions.csv`, `sdkconfig.defaults`, `dependencies.lock`, `tools/validate.sh` |

Preserve NVS `0x9000/0x6000`, PHY `0xF000/0x1000`, factory `0x10000/0x2F0000`, and FAT recording storage `0x300000/0x500000`. See [firmware layout](development/engineering/firmware-layout.md). Inherited three-partition template and BLUFI examples are reference material, not the shipped app's layout or provisioning workflow.

## Release baseline recorded on 2026-10-04

- Source: [openbrt/netdisk-recording-badge](https://github.com/openbrt/netdisk-recording-badge), source baseline commit `b81cf3726151736093d4128b180263aeebd547e3`.
- Firmware: [0.4.4 release](https://github.com/openbrt/netdisk-recording-badge/releases/tag/v0.4.4-netdisk-recording-badge), tag `v0.4.4-netdisk-recording-badge`. The tag identifies the initial standalone publication; later documentation and website fixes are on `main`.
- Website: [online installation](https://openbrt.github.io/netdisk-recording-badge/#install) and [USB Wi-Fi setup](https://openbrt.github.io/netdisk-recording-badge/#wifi-config). Desktop Chrome/Edge uses USB; the Wi-Fi password is sent only to the badge.

| Approved release artifact | Offset | SHA-256 |
| --- | --- | --- |
| `FoloToy-AI-Passport-full.bin` | `0x0` | `f074dacf0e87bf9680698a597ff42fd1cf59695da06c63fba77d9b11e8814b6c` |
| `FoloToy-AI-Passport.bin` | `0x10000` | `a2ebafc68a96f84c63f29d42374afad79add76c11696097c7cac18d2bdefa394` |

The public source contains no actual owner Baidu application credentials. The approved distributed firmware embeds the owner's application configuration with their authorization. Public CI and the default local validation header use placeholders: their binaries are not a replacement for the approved release. A rebuild has its own artifact identity and requires its own acceptance.

`build/FoloToy-AI-Passport-full.bin` is the latest successful local gate output, not necessarily the published firmware. Identify every artifact through its SHA-256 and matching archived ELF. Keep all debugging bundles private. A full merged flash can reset NVS; only a compatible application update without erase preserves the relevant stored data. Do not add a full-chip erase to routine maintenance.

## Evidence and remaining work

The standalone build and C/Python/browser protocol tests passed before workspace migration. Public release download identity, source credential exclusion, Pages deployment, and public-page assets were checked. Firmware 0.4.4 passed physical saved-network short-list, cancellation, and USB reboot regressions. Approximate ten-minute segmentation/upload/downloaded WAV verification belongs to the earlier 0.4.3 firmware, not a 0.4.4 long-recording test.

The 2026-10-06 upload-fix revision passed a battery run from 71% charge, verifying at least 2 hours 48 minutes 13 seconds of capture/writing and 168 complete one-minute cloud files. Full-charge recording with concurrent uploads is estimated at about four hours under the tested conditions. This result belongs to a different image from the published 0.4.4 release above; see [recording endurance](recording-endurance.md) for hashes and limits. The firmware descriptor still reads 0.4.4. Do not replace the tested artifact with an arbitrary fresh build or attribute its result to the older image.

Remaining acceptance work: physical browser first-install and new-network USB setup; a direct full-charge-to-shutdown run; exact stopping time and last unclosed segment recovery; actual network loss; listening across segment boundaries; full on-device guide readability; native Windows build commands. A simulated serial test or successful build does not establish these device results.

Community submission is a separate publication channel. Read the owner's local evidence for its project/revision and last observed state, and query current status when working on it. GitHub publication does not mean community approval. Do not create a duplicate project to retry an uncertain upload.
