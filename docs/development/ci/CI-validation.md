[简体中文](CI-validation.zh_CN.md) · **English**

# Independent project CI validation

`static-checks.yml` runs `./tools/validate.sh --static` on PRs, main pushes, and manual dispatch. It covers documents, workflows, C/Python tests, and Node.js web protocol tests.

`firmware-checks.yml` builds and verifies merged firmware with ESP-IDF 5.5.3 on PRs, main pushes, and manual dispatch. Paths under docs, plays, skills, and Markdown are filtered; asset changes still build. A path-filtered job may be absent, so do not require it for every PR without revisiting filtering.

Public CI copies placeholder credentials from `main/kuku_baidu_keys.example.h`, requires no owner Secret, and can build fork PRs. It uploads no firmware artifacts and creates no automatic releases. Compilation does not establish working Baidu authorization.

For local reproduction, prepare the environment and local credentials using the [build guide](../../build.md), then run the shared gate. Actions use full commit SHAs. Historical `sync-main.yml` runs only on GitHub forks; this independent repository does not automatically synchronize upstream.
