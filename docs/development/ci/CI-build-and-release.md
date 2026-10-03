[简体中文](CI-build-and-release.zh_CN.md) · **English**

# Build and release

`build-firmware.yml` is a manual placeholder-credential compilation check. It uploads no artifacts and creates no automatic releases. See [CI validation](CI-validation.md) for PR checks and the [build guide](../../build.md) for your own usable firmware.

For an official release, the maintainer completes the local gate, relevant physical acceptance, and archive verification before uploading binaries explicitly authorized for public distribution. Source excludes real Baidu application Secrets; the owner requests that distributed 0.4.4 firmware include their application configuration. Matching ELF/MAP archives stay local. Never upload personal Wi-Fi settings, user authorization, recordings, or unsanitized logs.

Tags use `v<version>-netdisk-recording-badge`. Update the bilingual independent release notes, distribute a full image at 0x0, a compatible application update at 0x10000, and checksums. The full image overwrites NVS; application updates require compatible partitions and no erase.

`pages.yml` runs web tests on releases, relevant main pushes, or manual dispatch. It downloads the release specified by `site/release.json`, verifies sizes, SHA-256 values, and manifest offsets, then deploys GitHub Pages. Firmware stays outside Git history. Version updates change that record and both manifests together, using the same accepted and reviewed binaries.
