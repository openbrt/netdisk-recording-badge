[English](session-handoff.md) · **简体中文**

# 网盘录音工牌会话接续说明

这是从维护中的工牌项目抽出的独立录音应用，后续开发在本仓库继续。`main` 分支已经是应用，不是干净的硬件测试模板。为主机回归保留的 demo 文件不参与 `main/CMakeLists.txt` 的固件构建；编译公开源码不需要访问原私有仓库。

## 新会话从这里开始

1. 读取根目录 [AGENTS.md](../AGENTS.md)，执行 `git status --short --branch`，保留已有修改。
2. 阅读本说明与[编译指南](build.zh_CN.md)。作者本机的 `.local/session-handoff.zh_CN.md` 另记录本机路径、正式发布归档与验收证据，已被 Git 忽略。
3. 根据[操作指南](kuku-badge-user-guide.zh_CN.md)和实际代码核对当前行为。修改操作或功能时，同步更新中英文指南与屏上操作指南。
4. 按任务读取工程资料，只使用匹配的已安装 Passport 技能。交付前运行完整门禁，分别报告构建、主机测试、真机测试和未验证事项。

## 当前功能与源码入口

工牌录制 WAV，每分钟保存一段并后台上传百度网盘；支持账号授权、网盘浏览、本地回放、JPG 查看和保存工牌照片。断网时保存并停止录音；恢复联网后可补传，续录需要用户再次按键。

| 范围 | 入口 |
| --- | --- |
| 启动、存储、应用共享状态 | `main/main.c`、`main/kuku_app.h` |
| 页面、按键、状态与屏上指南 | `main/kuku_ui.c` |
| 采集、分段与 WAV 完整性 | `main/kuku_rec.c`、`main/kuku_wav.c`、`main/kuku_rec_filename.c` |
| 网盘授权、传输与路径 | `main/kuku_baidu.c`、`main/kuku_baidu_auth_link.c`、`main/kuku_cloud_path.c` |
| Wi-Fi 与 USB 串口命令 | `main/kuku_wifi.cc`、`main/kuku_test.c`、`components/esp-wifi-connect/` |
| 照片与 JPG | `main/kuku_image.c`、`assets/fonts/` |
| 硬件驱动 | `components/bsp/` |
| 在线烧写与 USB 配网 | `site/`、`tests/test_web_serial.mjs`、`tools/prepare_site.py` |
| 配置与门禁 | `partitions.csv`、`sdkconfig.defaults`、`dependencies.lock`、`tools/validate.sh` |

保留 NVS `0x9000/0x6000`、PHY `0xF000/0x1000`、factory `0x10000/0x2F0000`、FAT 录音存储 `0x300000/0x500000`，详见[固件布局](development/engineering/firmware-layout.zh_CN.md)。继承的最简三分区和 BLUFI 示例仅作参考，不是本应用的分区或配网流程。

## 2026-10-04 记录的发布基线

- 源码：[openbrt/netdisk-recording-badge](https://github.com/openbrt/netdisk-recording-badge)，源码基线提交 `b81cf3726151736093d4128b180263aeebd547e3`。
- 固件：[0.4.4 Release](https://github.com/openbrt/netdisk-recording-badge/releases/tag/v0.4.4-netdisk-recording-badge)，标签 `v0.4.4-netdisk-recording-badge`。标签对应首次独立发布；之后的文档和网站修正在 `main`。
- 网页：[在线烧写](https://openbrt.github.io/netdisk-recording-badge/#install)、[USB Wi-Fi 配网](https://openbrt.github.io/netdisk-recording-badge/#wifi-config)。电脑 Chrome/Edge 经 USB 操作，Wi-Fi 密码只发到工牌。

| 已授权发布产物 | 偏移 | SHA-256 |
| --- | --- | --- |
| `FoloToy-AI-Passport-full.bin` | `0x0` | `f074dacf0e87bf9680698a597ff42fd1cf59695da06c63fba77d9b11e8814b6c` |
| `FoloToy-AI-Passport.bin` | `0x10000` | `a2ebafc68a96f84c63f29d42374afad79add76c11696097c7cac18d2bdefa394` |

公开源码不含作者真实百度应用凭据；已授权发布的固件嵌入作者应用配置。公开 CI 与本机默认验证头文件使用占位值，其二进制不能替代正式发布固件。重新编译得到自己的固件身份，需要单独验收。

`build/FoloToy-AI-Passport-full.bin` 是最近一次成功门禁的本地产物，不一定是发布固件。每次以 SHA-256 和配套归档 ELF 确认身份，调试归档保持私有。完整合并镜像可能重置 NVS；只有兼容的应用更新且不擦除，才能保留相应存储数据。不得把全片擦除加入常规维护。

## 验证证据与待办

迁移前独立项目构建及 C/Python/网页协议测试均通过；已检查公开固件下载身份、源码凭据排除、Pages 部署与线上页面资源。0.4.4 完成已存网络短列表、取消操作和 USB 重启实机回归。约十分钟自动分段、上传与下载 WAV 内容验收属于此前 0.4.3，不可当作 0.4.4 长录音测试。

2026-10-06 上传修正版从71%电量开始，验证连续采集与写入至少2小时48分13秒，网盘保存168个完整一分钟录音。按本次条件估算，满电、边录边上传约4小时。此结果对应的镜像与上方已发布的 0.4.4 不同；哈希和限制见[录音续航](recording-endurance.zh_CN.md)。固件内部版本仍为 0.4.4，不得随意用新编译产物替换已测试镜像，或将结果套用于旧镜像。

待验收：浏览器首次烧写与新网络 USB 配网；满电到关机直接测试；精确停止时长及最后未闭合段恢复；实际断网；跨分段听感；屏上指南完整可读性；原生 Windows 编译命令。模拟串口测试和编译通过不能替代这些真机结果。

社区提交是另一条发布渠道。项目、修订及最后观察状态见作者本机证据；处理社区发布时需查询最新状态。GitHub 已公开不代表社区已审核通过；上传状态不明时不能为重试另建玩法。
