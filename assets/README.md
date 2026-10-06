<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

KuKu's Chinese UI uses `fonts/SourceHanSansSC-Regular.otf` from Adobe Source Han Sans (release branch), licensed under the SIL Open Font License 1.1 in `fonts/SourceHanSans-LICENSE.txt`. `python3 tools/generate_kuku_fonts.py` invokes pinned lv_font_conv 1.5.3 to generate uncompressed 4bpp fonts at 14/20/28px with printable ASCII and the Chinese inventory in `fonts/kuku-glyphs.json`. main/CMakeLists.txt compiles the sources; kuku_ui.c explicitly selects them and checks coverage at boot. Fixed UI text is covered; arbitrary Chinese Wi-Fi names are not guaranteed by the subset.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.

`images/netdisk-recording-cover.png`: 1086 × 1448 PNG, an AI-generated illustrative cover supplied and authorized for this project publication by the owner on 2026-10-04. It is not a device screenshot. The Pages build copies the original file into the website; firmware does not embed it.

`images/netdisk-recording-badge-photo.png`: 1440 × 1920 PNG, the creator-supplied device photo used for community publication on 2026-10-05. The existing 3:4 gallery copy preserves the photograph with side margins. It is a historical appearance reference, not evidence of four-hour endurance or complete guide readability. It is used for project review/publication and is not embedded in firmware; no separate image license was supplied.
