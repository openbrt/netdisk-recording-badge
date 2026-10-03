[简体中文](build.zh_CN.md) · **English**

# Build Netdisk Recording Badge

Target FoloToy AI Passport: ESP32-C3, 8 MB Flash, no PSRAM. Use ESP-IDF **5.5.3**. To use the owner's firmware, choose the [online installer](https://openbrt.github.io/netdisk-recording-badge/) without installing a toolchain.

## 1. Prepare the environment and source

Install Git, Python 3, a host C compiler, and ESP-IDF system dependencies as described in the [environment guide](development/engineering/environment-setup.md). Basic macOS/Linux commands follow. On Windows, install 5.5.3 with Espressif's installer and run project commands in its ESP-IDF terminal.

```bash
git clone --branch v5.5.3 --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.5.3
cd esp-idf-v5.5.3
./install.sh esp32c3
. ./export.sh
cd ..
git clone https://github.com/openbrt/netdisk-recording-badge.git
cd netdisk-recording-badge
idf.py --version
```

The version must report `ESP-IDF v5.5.3`. Activate the installation's `export.sh` in each new terminal. The environment guide covers official mainland-China mirrors and failed downloads. Managed Components are fetched according to `dependencies.lock`; the first build needs Internet access. Web serial tests additionally require Node.js 18 or newer.

## 2. Configure local Baidu application credentials

Create a Baidu Open Platform application supporting Netdisk device authorization and obtain your own App Key and Secret. Copy the template:

```bash
cp main/kuku_baidu_keys.example.h main/kuku_baidu_keys.h
```

Replace the two placeholder values in your local header using an editor:

```c
#pragma once
#define KUKU_BAIDU_APPKEY "REPLACE_WITH_YOUR_BAIDU_APP_KEY"
#define KUKU_BAIDU_SECRET "REPLACE_WITH_YOUR_BAIDU_APP_SECRET"
```

Git ignores `main/kuku_baidu_keys.h`; never upload or commit it. Placeholder values cannot produce usable authorization. Published 0.4.4 firmware includes the owner's application configuration, whose real values are excluded from source. Your own build embeds the credentials you enter, so firmware is not a confidential container. Configure personal Wi-Fi passwords and user Netdisk authorization on the badge, without putting them in source.

## 3. Build, validate, and package

From the project root:

```bash
./tools/validate.sh --static
node --test tests/test_web_serial.mjs
./tools/validate.sh --firmware
# Or run the complete gate once, with Node.js available:
./tools/validate.sh
```

The complete gate checks documentation/workflows, runs C/Python/web protocol host tests, builds with an isolated temporary `sdkconfig`, and verifies full-image offsets, partitions, sizes, and content. Tracked `sdkconfig.defaults`, `partitions.csv`, and `dependencies.lock` define the configuration. It does not read your root `sdkconfig` or flash hardware.

The successful merged image is `build/FoloToy-AI-Passport-full.bin`, for flashing at `0x0` on a blank device or an intentional complete refresh. Matching application, ELF, MAP, partition table, and identity records are retained under `build/firmware/<full-image-SHA-256>/`. Verify the archive:

```bash
python3 tools/archive_firmware.py verify build/firmware/<full-image-SHA-256>
```

In a native Windows ESP-IDF terminal, build and verify the images directly:

```text
idf.py set-target esp32c3
idf.py build
idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build
python tools/archive_firmware.py create build
```

These commands do not run host tests. The complete Bash gate also needs Bash, a host C compiler, Python, and Node.js; run it on macOS/Linux or a configured WSL environment. Native Windows compilation does not require WSL just to use the firmware.

Keep `build/` out of Git. Rebuilding with your own credentials changes firmware hashes; the owner's binary acceptance results do not apply to your new build. Compilation does not establish physical behavior.

## 4. Incremental development and flashing

For an incremental development build:

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p <your-badge-port> flash monitor
```

Use `set-target` for initial configuration or deliberate default regeneration, saving intentional local changes first. Select the port actually enumerated by your OS. Exit monitor with Ctrl+]. Incremental `flash` writes separate images; it preserves configuration only with a compatible partition layout and write ranges that avoid user data. Do not add `erase-flash` to routine commands.

The merged image overwrites padded gaps including NVS, potentially clearing Wi-Fi and Netdisk authorization. Never write the application-only image at `0x0`; this application's offset is `0x10000`. The website's application-only update requires a compatible 0.4.x layout of this project and no erase selection. See [firmware layout and data impact](development/engineering/firmware-layout.md).

After startup, follow the [user guide](kuku-badge-user-guide.md) for Wi-Fi, authorization, and recording acceptance. Public CI uses placeholder credentials to check compilation; it does not produce functional authorized releases. Official releases use locally accepted binaries explicitly authorized for publication by the owner.
