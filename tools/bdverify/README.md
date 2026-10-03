<p align="right"><strong>English</strong> · <a href="README.zh_CN.md">简体中文</a></p>

# Baidu Netdisk verification tool

`bdverify` independently lists and downloads recordings from the Netdisk Recording Badge application folder so a firmware upload can be checked against the downloaded file's MD5.

Install the Python `requests` package for CLI use. Credentials come from the `KUKU_BAIDU_APPKEY` and `KUKU_BAIDU_SECRET` environment variables, or from the Git-ignored `main/kuku_baidu_keys.h`. OAuth tokens are kept outside the repository in `~/.kuku-bdverify/token.json`.

```bash
python3 -m pip install requests
tools/bdverify/bdverify auth
tools/bdverify/bdverify ls
tools/bdverify/bdverify md5 RECORDING.WAV
```

`get` downloads a named recording to the Git-ignored `bd_pull/` directory. `rm` deletes one named file from the application folder; use it only when that removal is intended. `raw` prints a live access token.

Run the offline tests with `python3 tools/bdverify/test_bdverify.py`.
