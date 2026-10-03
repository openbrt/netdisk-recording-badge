<p align="right"><a href="README.md">English</a> · <strong>简体中文</strong></p>

# 百度网盘校验工具

`bdverify` 独立列出并下载网盘录音工牌应用目录里的录音，可将下载文件的 MD5 与固件上传记录比较。

命令行使用时需要安装 Python 的 `requests` 包。凭据来自环境变量 `KUKU_BAIDU_APPKEY` 和 `KUKU_BAIDU_SECRET`，或已被 Git 忽略的 `main/kuku_baidu_keys.h`。OAuth 令牌保存在仓库外的 `~/.kuku-bdverify/token.json`。

```bash
python3 -m pip install requests
tools/bdverify/bdverify auth
tools/bdverify/bdverify ls
tools/bdverify/bdverify md5 录音.WAV
```

`get` 将指定录音下载到被 Git 忽略的 `bd_pull/` 目录。`rm` 会删除应用目录中的指定文件，应仅在确需删除时使用。`raw` 会打印有效的访问令牌。

离线测试命令：`python3 tools/bdverify/test_bdverify.py`。
