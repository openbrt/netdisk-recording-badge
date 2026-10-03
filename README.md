[简体中文](README.zh_CN.md) · **English**

# Netdisk Recording Badge

Capture audio on a wearable badge, save a segment every minute, and upload completed segments to Baidu Netdisk in the background. Recordings are organized by date for transcription and summaries in Baidu Netdisk KuKu AI. Browse cloud files, play local recordings, preview JPG images, and save a badge photo.

**[Online installation and USB Wi-Fi setup](https://openbrt.github.io/netdisk-recording-badge/)** · [Download firmware 0.4.4](https://github.com/openbrt/netdisk-recording-badge/releases/tag/v0.4.4-netdisk-recording-badge) · [Build instructions](docs/build.md)

This independent project extracts the maintained recording application for FoloToy AI Passport. It includes application sources, drivers, build configuration, and tests, without requiring access to the original private repository.

## Get started

1. Connect the badge with a USB data cable. Open the installer in desktop Chrome or Edge and select the badge. A full installation overwrites network settings and authorization; compatible 0.4.x installations of this project can use the application-only update without selecting erase.
2. Close the installer and wait for reboot. Connect the badge in the same page's USB Wi-Fi section, enter a password-protected 2.4 GHz network's SSID and password, and save. Success requires a connected status and IP address. Check the password and retry on failure; enter another network to change it. The badge's Wi-Fi menu also supports setup.
3. Press OK to open the menu, enter cloud files, and scan the badge's QR code with a phone to authorize your own Baidu Netdisk account.
4. Hold OK on the home screen, or press DOWN and OK together, to record. Hold OK while recording to stop. Find recordings by date in the Netdisk application folder and use KuKu AI for transcription. Network loss saves and stops recording; saved files upload when the connection returns. Press again to resume recording.
5. Hold OK to leave a subpage. Hold DOWN on the home screen to turn off the display; any key wakes it.

See the [user guide](docs/kuku-badge-user-guide.md) for detailed button, file, and network operations.

## Source and firmware

Public source excludes the owner's actual Baidu application Secret. Distributed firmware includes the owner's application configuration with their authorization; users still authorize their own Baidu account. The website has no application-Secret replacement form. Developers can configure their own application credentials in a Git-ignored local header using the [build guide](docs/build.md). Wi-Fi passwords travel only over USB from the browser to the badge.

## Validation scope

Firmware 0.4.4 passed build, host tests, and physical saved-network short-list regression. Earlier 0.4.3 testing covered approximately ten minutes of continuous capture, segmentation, upload, and downloaded WAV content verification. Multi-hour recording, actual network/power loss, and listening across segment boundaries remain unverified. The website has separate protocol and simulated serial tests; physical first-install and browser USB provisioning still need acceptance.

## Development and attribution

The [bilingual build guide](docs/build.md) covers setup, credentials, complete validation, and outputs. The [documentation index](docs/README.md) retains hardware and engineering references. `main/CMakeLists.txt` builds only the recording application; reference demo sources support host regression tests.

Based on [FoloToy AI Passport](https://github.com/FoloToy/ai-passport), preserving its MIT license and copyright. The cover is an AI-generated illustration, not a device screenshot. Browser installation uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/).
