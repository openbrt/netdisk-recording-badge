[English](README.md) · **简体中文**

# 网盘录音工牌

随身记录声音，每分钟自动保存一段并后台上传百度网盘。录音按日期整理，方便在百度网盘库库 AI 中转写、提取摘要；工牌还能浏览网盘文件、回放本地录音、查看 JPG 并保存工牌照片。

**[在线烧写与 USB 配网](https://openbrt.github.io/netdisk-recording-badge/)** · [下载 0.4.4 固件](https://github.com/openbrt/netdisk-recording-badge/releases/tag/v0.4.4-netdisk-recording-badge) · [编译说明](docs/build.zh_CN.md)

这是从维护中的录音工牌应用抽取的独立项目，运行于 FoloToy AI Passport。应用源码、驱动、构建配置和测试已独立保存，不依赖访问原私有仓库。

## 上手

1. 用 USB 数据线连接工牌，在电脑 Chrome 或 Edge 打开在线烧写页，选择设备安装固件。首次安装的完整镜像会覆盖原配网和授权；兼容本项目 0.4.x 的设备可选择仅更新应用，且不要勾选擦除。
2. 关闭烧写窗口，等待重启。在同页“配置 Wi-Fi”连接 USB 工牌，填写有密码的 2.4 GHz 网络名称和密码，点击“保存并连接”。显示已连接及 IP 才表示成功；失败时检查密码后重试，换网时重新填写保存。也可在工牌“无线网络”菜单配网。
3. 短按 OK 打开菜单，进入“网盘文件”，用手机扫描工牌二维码，授权自己的百度网盘。
4. 主页长按 OK 或同时按下键与 OK 开始录音；录音中长按 OK 停止。在网盘应用目录中按日期找到录音，再用库库 AI 转写。断网时保存并停止，联网后补传；续录需再次按键。
5. 子页长按 OK 返回；主页长按下键熄屏，任意键唤醒。

完整按键、文件和网络操作见[操作指南](docs/kuku-badge-user-guide.zh_CN.md)。

## 源码与固件

公开源码不包含作者的百度应用 Secret。发布固件按作者授权包含其百度应用配置，用户仍需授权自己的百度账号；网页不提供替换应用 Secret 的入口。开发者可按[编译说明](docs/build.zh_CN.md)在被 Git 忽略的本地头文件中配置自己的应用凭据。Wi-Fi 密码只在浏览器与工牌之间经 USB 传输，不上传服务器。

## 验证范围

0.4.4 固件已完成构建、主机测试和已存网络短列表实机回归。此前 0.4.3 已验收约十分钟连续录音、自动分段、上传与下载 WAV 内容核对；多小时录音、实际断网断电及跨分段听感尚未验收。在线页面另有协议与串口模拟测试；浏览器首次烧写和 USB 配网的实机操作仍需验收。

## 开发与来源

[中英文编译说明](docs/build.zh_CN.md)包含环境安装、凭据配置、完整校验与产物位置。[文档索引](docs/README.zh_CN.md)保留硬件和工程参考。`main/CMakeLists.txt` 只构建录音应用；参考 demo 源文件用于主机回归测试。

基于 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport)，保留其 MIT 许可与版权声明。页面封面为 AI 生成示意图，非实机截图。浏览器烧写使用 [ESP Web Tools](https://esphome.github.io/esp-web-tools/)。
