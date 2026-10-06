[English](README.md) · **简体中文**

# 私有录音续航后台

Python 标准库收集器通过可信局域网接收工牌的鉴权遥测，不接收音频，也不连接百度。每批先追加 `samples.jsonl` 再确认接收，新样本到达及每 30 秒更新 `report.json`。配置、输出和密钥私下存放，例如 `.local/endurance/`。

创建私有 JSON 配置，包含 `bind`（电脑局域网 IPv4 地址）、`port`（例如 8765）、`token`（随机 16～64 位 ASCII 字母数字密钥）和 `output`（私有目录），权限设为 0600。启动：

```text
python3 tools/endurance/collector.py --config <private-config.json>
```

保持电脑唤醒、进程运行。此 HTTP 服务仅用于可信局域网，不向互联网暴露。HTTP 中的密钥未加密。工牌的 HTTPS 地址通过 ESP-IDF 证书包验证，但此小型收集器不配置 TLS。

确认工牌 Wi-Fi 能访问电脑后，通过 USB 发送：

```text
ENDURANCE ARM http://<computer-lan-ip>:8765/telemetry <private-token>
ENDURANCE STATUS
```

配置不持久化。ARM 返回 0 表示已入队，-1 表示输入无效/诊断不可用，-2 表示忙，-3 表示修改地址/密钥需要重启，-4 表示后台永久拒绝需要重启后修正配置。数据主机连接稳定十秒后产生 `host_ready`，报告显示 `ready_to_unplug`。拔线前确认最新样本已接收、电量/电压有效、Wi-Fi/网盘就绪且采集前进。主机断开稳定五秒后，后台收到 `usb_host_detached`。拔线不自动开始采集。诊断 `REC` 命令最多十分钟，请使用正常录音按键。`ENDURANCE OFF` 停止收集而不停止录音。OFF 后可用相同地址/密钥重新 ARM 创建下一轮；修改配置则重启。

发送端保存六个待确认样本及 32 项队列，使用有上限的退避重试，并上报丢样数。永久 HTTP 4xx 拒绝（不含 408/429）关闭测试遥测，不影响录音；STATUS 显示 `http_error`，重启后修正配置。采样独立于 HTTP，不改变背光。字段包括启动/轮次/序号、单调时钟、固件 ELF 身份、电量/电压、主机/录音/网络状态、设置的亮度、64 位 PCM 字节进度、已关闭分段数、空间、堆及录音停止原因。`stage` 为 0 等待主机、1 就绪、2 已拔线、3 已关闭；录音原因是 0 运行、1 正常/手动停止、2 网络、3 空间、4 采集、5 存储 I/O。仅数据主机断开不能证明 VBUS 断电，应保持电脑唤醒并完全拔线。

报告排除拔线/开始录音/亮度变化后五分钟，要求至少 30 分钟有效区间、电量下降十个百分点、足够采样覆盖、PCM 前进及回归/稳健/前后半区间斜率一致。报告保留 10% 电量的预计剩余/满电时长，并单列到 0% 的外推。范围是模型敏感性，不是已校准置信区间。跨度十分钟的三次一致报告标记 `short_test_sufficient`，进程不自动停录。遥测过期冻结结果，不能证明电量耗尽。另行校验下载 WAV。完整流程见[测试方案](../../docs/development/engineering/recording-endurance-test-plan.zh_CN.md)。

测试已接入 `./tools/validate.sh --static`；可单独运行 `python3 tests/test_endurance_collector.py`，验证协议与模拟曲线，不验证实物电池校准或 USB 行为。

网盘批次上传占用网络内存期间，遥测 HTTP 请求排队等待；采样继续，并保留事件原始时间。发送/采样任务栈分别为 4096/3072 字节。USB `STATS` 提供 PCM 计数、固件身份、堆和可取得的各任务栈最低余量。跨 USB 数据包的命令组装到换行后才执行；超长或含 NUL 的整行拒绝执行。
