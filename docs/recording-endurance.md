[简体中文](recording-endurance.zh_CN.md) · **English**

# Continuous recording endurance

**With a full battery, a healthy network connection and uploads running alongside recording, estimated continuous recording time is about four hours.**

This estimate applies to the upload-fix firmware revision tested on 2026-10-06. It is derived from the battery curve, rather than a direct four-hour run from a full charge. It does not establish endurance for the original published 0.4.4 image.

## What the test established

| Result | Evidence |
| --- | --- |
| Continuous recording lower bound | **2 hours 48 minutes 13 seconds**, starting at **71%** battery; actual captured and written PCM counters matched |
| Completed cloud recordings | **168 one-minute WAV files**, totaling **2 hours 48 minutes**; the final seven completed files passed downloaded WAV format, frame-count and length checks |
| Full charge to displayed 0% | **About 3.8 hours**, rounded to about four hours; a linear projection, not a measured shutdown time |
| Full charge with 10% reserve | **About 3.4 hours**, with model sensitivity **3.2–4.4 hours**; this range is not a statistical confidence interval |
| End of the battery run | Battery shutdown was confirmed by the tester; the exact shutdown time and final partial segment were not recovered |

The estimate used more than 162 minutes of eligible samples, approximately 97.5% sample coverage, and the collector's existing progress, load-stability and slope-consistency checks. Earlier USB-powered recordings, manually stopped recordings and idle battery use were excluded. The last valid sample still showed recording and Wi-Fi active, with no reported capture stop or telemetry queue drops. Missing later telemetry was not treated as proof of shutdown.

## Conditions and firmware identity

- Wi-Fi remained connected, with completed recordings uploaded to Baidu Netdisk during capture.
- Audio was 16 kHz, 16-bit mono PCM in WAV files, with one-minute segmentation.
- Backlight was briefly 100% at startup and then remained at 15%; transition samples were excluded from the estimate.
- The private wireless endurance diagnostic was enabled. Different brightness, network/upload delays, battery condition or workload can change runtime.
- The firmware descriptor remains `0.4.4`; identify this tested revision by its hashes, not the version string alone.

| Tested artifact | SHA-256 |
| --- | --- |
| Merged firmware | `861c1bdd05c01c54ee257d23fe110cbf4742a2a41ab0ddfadbbc16c460ec23f1` |
| Application firmware | `8445c6ed8b04d7000a1414ffbca4d862b68066de0c39ecee74ec526f4a4a4d0f` |
| Matching ELF identity | `ab3c9602a0818f8f8b64989d40699cc47ac86eb3a4cb6225658b67829a192d62` |

Before the battery run, this revision passed a 371.936-second USB-powered recording check: all seven segments uploaded, their confirmed local copies were reclaimed, and downloaded WAVs matched device MD5 values. Build and host checks passed. These short-run MD5 checks do not constitute whole-session integrity verification for the longer battery run.

## Remaining limits

A full-charge-to-shutdown run, exact final duration, recovery of the last unclosed segment, whole-session downloaded integrity and listening across segment boundaries remain unverified. About four hours is an estimate under the stated conditions, not a guaranteed minimum or absolute maximum. Recording can stop earlier after a network loss or if uploads cannot keep pace and local storage fills.

See the [user guide](kuku-badge-user-guide.md) for controls and the [endurance test plan](development/engineering/recording-endurance-test-plan.md) for the measurement method. Community review and GitHub release status are separate from this test result.
