[English](CI-build-and-release.md) · **简体中文**

# 构建与发布

`build-firmware.yml` 仅手动执行占位凭据构建，用于检查编译，不上传 artifact 或自动发布。日常 PR 检查见[CI 校验](CI-validation.zh_CN.md)。开发者编译自己的可用固件见[编译说明](../../build.zh_CN.md)。

正式发布由维护者在本地完成完整 gate、相应实机验收和镜像归档核对，再上传明确授权公开的固件。源码不包含真实百度应用 Secret；发布的 0.4.4 固件按作者要求包含其应用配置。匹配的 ELF/MAP 调试归档保留本地，不自动发布。不要把个人 Wi-Fi、用户授权、录音或未脱敏日志上传。

标签使用 `v<version>-netdisk-recording-badge`。更新独立的中英文更新日志，提供完整镜像（0x0）、兼容应用更新镜像（0x10000）及校验值。完整镜像会覆盖 NVS；应用更新需兼容分区且不能擦除。

`pages.yml` 在发布、站点相关 main push 或手动执行时运行网页测试，然后从 `site/release.json` 指定的 Release 下载固件，核对大小、SHA-256 与 manifest 偏移后部署 GitHub Pages。固件不进入 Git 历史。更新版本时同步该记录及两个 manifest，只能使用验收和审核过的同一份二进制。
