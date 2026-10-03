[English](CI-validation.md) · **简体中文**

# 独立项目 CI 校验

`static-checks.yml` 在 PR、main push 与手动执行时运行 `./tools/validate.sh --static`，包括文档、工作流、C/Python 与 Node.js 网页协议测试。

`firmware-checks.yml` 在 PR、main push 与手动执行时，用 ESP-IDF 5.5.3 编译并校验完整镜像。docs、plays、skills 与 Markdown 变化被路径过滤；资源变化仍构建。不要把路径过滤后可能不存在的 job 设为所有 PR 必需检查。

公开 CI 只复制 `main/kuku_baidu_keys.example.h` 的占位凭据，不需要作者 Secret，fork PR 也可构建。它不上传固件 artifact，也不自动创建发布版本；编译成功不能证明百度授权可用。

本地复现请先按[编译说明](../../build.zh_CN.md)准备环境和本地凭据，再运行统一 gate。所有 Action 固定完整 SHA。历史 `sync-main.yml` 仅在 GitHub fork 上执行；本仓库为独立仓库，不自动同步上游。
