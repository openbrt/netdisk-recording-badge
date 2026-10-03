[English](build.md) · **简体中文**

# 网盘录音工牌编译说明

编译目标为 FoloToy AI Passport（ESP32-C3、8 MB Flash、无 PSRAM），固定使用 ESP-IDF **5.5.3**。若只需使用作者的固件，直接[在线烧写](https://openbrt.github.io/netdisk-recording-badge/)，无需配置编译环境。

## 1. 准备环境和源码

安装 Git、Python 3、C 编译器及 ESP-IDF 所需系统依赖，详见[环境安装指南](development/engineering/environment-setup.zh_CN.md)。macOS/Linux 的基本命令如下；Windows 使用 Espressif 安装器安装 5.5.3，在对应 ESP-IDF 终端执行项目命令。

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

版本应显示 `ESP-IDF v5.5.3`。每次打开新终端都先执行安装目录中的 `export.sh`；国内官方镜像和下载失败处理见环境指南。编译过程中 Managed Components 自动按 `dependencies.lock` 下载，首次构建需联网。仅网页串口测试另需 Node.js 18 或更新版本。

## 2. 本地配置百度应用凭据

在百度开放平台创建支持网盘设备授权的应用，取得自己的 App Key 和 Secret。复制模板：

```bash
cp main/kuku_baidu_keys.example.h main/kuku_baidu_keys.h
```

用编辑器在本地头文件中替换两个占位值：

```c
#pragma once
#define KUKU_BAIDU_APPKEY "REPLACE_WITH_YOUR_BAIDU_APP_KEY"
#define KUKU_BAIDU_SECRET "REPLACE_WITH_YOUR_BAIDU_APP_SECRET"
```

`main/kuku_baidu_keys.h` 已被 Git 忽略；不要上传或提交该文件。不要用公开模板占位值作为可用固件的凭据。发布的 0.4.4 固件包含作者应用配置；公开源码不提供这些真实值。自行编译的固件会嵌入你填写的凭据，不能视为保密容器；个人 Wi-Fi 密码和网盘用户授权在工牌上配置，无需写入源码。

## 3. 编译、校验和打包

从项目根目录执行：

```bash
./tools/validate.sh --static
node --test tests/test_web_serial.mjs
./tools/validate.sh --firmware
# 或在 Node.js 可用时，一次运行完整 gate：
./tools/validate.sh
```

完整 gate 会检查文档与工作流、运行 C/Python/网页协议主机测试，用独立临时 `sdkconfig` 编译，生成并核对完整镜像的偏移、分区、大小与内容。配置以仓库的 `sdkconfig.defaults`、`partitions.csv` 和 `dependencies.lock` 为准。不会读取个人根目录 `sdkconfig`，不会烧写设备。

成功后完整固件位于 `build/FoloToy-AI-Passport-full.bin`，用于从 `0x0` 写入空设备或有意完整重装。配套应用镜像、ELF、MAP、分区表和身份记录保存在 `build/firmware/<完整镜像 SHA-256>/`。核对归档：

```bash
python3 tools/archive_firmware.py verify build/firmware/<完整镜像-SHA-256>
```

原生 Windows ESP-IDF 终端可直接编译并验证镜像：

```text
idf.py set-target esp32c3
idf.py build
idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build
python tools/archive_firmware.py create build
```

这些命令不运行主机测试。完整 Bash gate 还需要 Bash、主机 C 编译器、Python 和 Node.js，可在 macOS/Linux 或配置好的 WSL 环境运行；Windows 原生编译不需要为了使用固件而安装 WSL。

`build/` 不提交到 Git。使用自己的凭据重新编译后，固件校验值会与作者发布版本不同；不能沿用作者二进制的验收结论。编译通过不代表实机功能通过。

## 4. 增量开发与烧写

需要常规增量构建时：

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p <你的工牌串口> flash monitor
```

`set-target` 用于首次配置或有意重新生成默认配置；有本地配置时先保存需要的改动。串口由系统实际枚举确定。退出 monitor 用 Ctrl+]。增量 `flash` 采用分段写入，只有兼容分区且写入范围不覆盖用户数据时才能保留配置；不要加入 `erase-flash` 作为日常步骤。

完整 `full.bin` 会覆盖包含 NVS 的镜像间隙，可能清除 Wi-Fi 和网盘授权。不要把应用镜像写到 `0x0`；当前应用偏移是 `0x10000`。网页“仅更新应用”只适用于本项目 0.4.x 的兼容布局，且不能勾选擦除。详情见[固件布局与数据影响](development/engineering/firmware-layout.zh_CN.md)。

启动后通过[操作指南](kuku-badge-user-guide.zh_CN.md)配网、扫码授权并验证录音。公开 CI 使用占位凭据，只检查编译，不生成可授权的发布版本；正式发布使用作者本地验收后明确授权公开的二进制。
