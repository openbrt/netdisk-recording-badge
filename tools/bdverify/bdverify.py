#!/usr/bin/env python3
"""bdverify —— 网盘录音工牌 网盘沙盒闭环校验工具。

工牌固件通过百度网盘开放平台 (xpan) 把录音分片上传到注册应用对应的网盘沙盒目录。
本工具用同一套应用凭证独立走一次设备码授权，之后可列目录 / 下载 / 校验
下载内容 md5，让 Codex 在调试固件上传时无需碰浏览器即可闭环验证写入结果。

凭证来源(优先级从高到低):
  1) 环境变量 KUKU_BAIDU_APPKEY / KUKU_BAIDU_SECRET
  2) 固件里的 main/kuku_baidu_keys.h (gitignore, 与工牌同一份)

token 缓存: ~/.kuku-bdverify/token.json  (refresh_token 单次滚动, 自动续期)

子命令:
  auth              设备码授权(首次或 refresh 失效时). 屏显 URL + user_code
  ls [子路径]       列出应用目录或指定子路径的文件
  tail [n]          按 mtime 显示最近 n 个文件(默认 1), 适合刚上传后即查
  get <名/路径>     下载文件到 ./bd_pull/ 并打印本地绝对路径
  md5 <名/路径>     下载并计算内容 md5 (与工牌本地分片 md5 对比用)
  rm <名/路径>      删除应用沙盒内指定文件
  cat <名/路径>     下载后把文本内容打印到 stdout (小文件调试用)
  raw               打印当前 access_token 与目录, 供手工 curl

退出码: 0 成功; 1 用法/文件错误; 2 需要授权(refresh 失效); 3 网络/API 错误。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import time
from pathlib import Path
from urllib.parse import quote

import requests

APP_DIR = "/apps/网盘录音工牌"
TOKEN_PATH = Path.home() / ".kuku-bdverify" / "token.json"
KEYS_HEADER = Path(__file__).resolve().parents[2] / "main" / "kuku_baidu_keys.h"
# 下载 dlink 时百度要求 UA 内含 pan.baidu.com，否则 403。
UA = "pan.baidu.com"
OPENAPI = "https://openapi.baidu.com/oauth/2.0"
XPAN = "https://pan.baidu.com/rest/2.0/xpan"
TIMEOUT = 30


def die(msg: str, code: int = 1) -> "None":
    print(f"bdverify: {msg}", file=sys.stderr)
    sys.exit(code)


def load_creds() -> tuple[str, str]:
    key = os.environ.get("KUKU_BAIDU_APPKEY")
    sec = os.environ.get("KUKU_BAIDU_SECRET")
    if key and sec:
        return key, sec
    if KEYS_HEADER.exists():
        text = KEYS_HEADER.read_text(encoding="utf-8", errors="replace")
        km = re.search(r'KUKU_BAIDU_APPKEY\s+"([^"]+)"', text)
        sm = re.search(r'KUKU_BAIDU_SECRET\s+"([^"]+)"', text)
        if km and sm:
            return km.group(1), sm.group(1)
    die(
        "找不到凭证。设置 KUKU_BAIDU_APPKEY / KUKU_BAIDU_SECRET，"
        f"或确保 {KEYS_HEADER} 存在。",
        2,
    )


def load_token() -> dict:
    if TOKEN_PATH.exists():
        try:
            return json.loads(TOKEN_PATH.read_text())
        except json.JSONDecodeError:
            pass
    return {}


def save_token(tok: dict) -> "None":
    TOKEN_PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp = TOKEN_PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(tok, ensure_ascii=False, indent=2))
    tmp.chmod(0o600)
    tmp.replace(TOKEN_PATH)


def _get(url: str, **kw) -> requests.Response:
    try:
        return requests.get(url, timeout=TIMEOUT, headers={"User-Agent": UA}, **kw)
    except requests.RequestException as e:
        die(f"网络错误: {e}", 3)


def _post(url: str, **kw) -> requests.Response:
    try:
        return requests.post(url, timeout=TIMEOUT, headers={"User-Agent": UA}, **kw)
    except requests.RequestException as e:
        die(f"网络错误: {e}", 3)


def do_auth(key: str, sec: str) -> dict:
    """设备码流程: 取 device_code → 屏显 → 轮询 device_token。"""
    r = _get(
        f"{OPENAPI}/device/code",
        params={"response_type": "device_code", "client_id": key, "scope": "basic,netdisk"},
    )
    d = r.json()
    if "device_code" not in d:
        die(f"device/code 失败: {d}", 3)
    interval = int(d.get("interval", 5))
    expires = int(d.get("expires_in", 300))
    print("=" * 56)
    print("  用手机/浏览器打开下面地址，输入验证码完成授权:")
    print(f"    地址: {d.get('verification_url', 'https://openapi.baidu.com/device')}")
    print(f"    验证码: {d.get('user_code', '-')}")
    print(f"    (二维码: {d.get('qrcode_url', '-')})")
    print("=" * 56, flush=True)
    deadline = time.time() + expires
    while time.time() < deadline:
        time.sleep(interval)
        tr = _get(
            f"{OPENAPI}/token",
            params={
                "grant_type": "device_token",
                "code": d["device_code"],
                "client_id": key,
                "client_secret": sec,
            },
        ).json()
        if "access_token" in tr:
            tok = {
                "access_token": tr["access_token"],
                "refresh_token": tr["refresh_token"],
                "expires_at": time.time() + int(tr.get("expires_in", 2592000)) - 600,
            }
            save_token(tok)
            print("授权成功，token 已缓存。", flush=True)
            return tok
        if tr.get("error") not in ("authorization_pending", "slow_down"):
            die(f"授权失败: {tr}", 2)
        print("  等待授权中…", flush=True)
    die("授权超时，重跑 `bdverify auth`。", 2)


def refresh(key: str, sec: str, tok: dict) -> dict:
    rt = tok.get("refresh_token")
    if not rt:
        return {}
    tr = _get(
        f"{OPENAPI}/token",
        params={
            "grant_type": "refresh_token",
            "refresh_token": rt,
            "client_id": key,
            "client_secret": sec,
        },
    ).json()
    if "access_token" not in tr:
        return {}
    tok = {
        "access_token": tr["access_token"],
        "refresh_token": tr.get("refresh_token", rt),
        "expires_at": time.time() + int(tr.get("expires_in", 2592000)) - 600,
    }
    save_token(tok)
    return tok


def ensure_token(key: str, sec: str, auto_auth: bool = False) -> str:
    tok = load_token()
    if tok.get("access_token") and tok.get("expires_at", 0) > time.time():
        return tok["access_token"]
    if tok.get("refresh_token"):
        tok = refresh(key, sec, tok)
        if tok:
            return tok["access_token"]
    if auto_auth:
        return do_auth(key, sec)["access_token"]
    die("未授权或 refresh 失效，先跑 `bdverify auth`。", 2)


def api_list(at: str, sub: str = "") -> list[dict]:
    d = (APP_DIR + "/" + sub.strip("/")).rstrip("/") if sub else APP_DIR
    r = _get(
        f"{XPAN}/file",
        params={"method": "list", "dir": d, "order": "time", "desc": 1, "access_token": at},
    ).json()
    errno = r.get("errno", 0)
    if errno == -9:
        # -9 = 目录不存在。工牌首次成功上传前沙盒目录还没被创建，视为空目录。
        return []
    if errno != 0:
        die(f"list errno={errno} ({r})", 3)
    return r.get("list", [])


def resolve_path(name: str) -> str:
    if name.startswith("/"):
        return name
    return f"{APP_DIR}/{name.lstrip('/')}"


def api_meta(at: str, path: str) -> dict:
    # filemetas accepts Baidu's numeric fs_id, not a pathname.  Resolve it from
    # the parent directory first so get/md5 work for both relative and absolute
    # paths inside the app sandbox.
    parent, _, name = path.rstrip("/").rpartition("/")
    if not parent or not name:
        die(f"无效文件路径: {path}", 1)
    if parent == APP_DIR:
        sub = ""
    elif parent.startswith(APP_DIR + "/"):
        sub = parent[len(APP_DIR) + 1 :]
    else:
        die(f"路径不在应用沙盒内: {path}", 1)
    entry = next(
        (
            it
            for it in api_list(at, sub)
            if it.get("path") == path or it.get("server_filename") == name
        ),
        None,
    )
    fs_id = entry.get("fs_id") if entry else None
    if not fs_id:
        die(f"找不到文件: {path}", 1)

    r = _get(
        f"{XPAN}/multimedia",
        params={
            "method": "filemetas",
            "access_token": at,
            "fsids": json.dumps([fs_id]),
            "dlink": 1,
        },
    ).json()
    lst = r.get("list") or []
    if not lst:
        die(f"找不到文件: {path} (resp={r})", 1)
    return lst[0]


def human(n: int) -> str:
    f = float(n)
    for u in ("B", "K", "M", "G"):
        if f < 1024 or u == "G":
            return f"{f:.0f}{u}" if u == "B" else f"{f:.1f}{u}"
        f /= 1024
    return f"{f:.1f}G"


def fmt_row(it: dict) -> str:
    ts = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(it.get("server_mtime", 0)))
    tag = "d" if it.get("isdir") else "-"
    size = "" if it.get("isdir") else human(it.get("size", 0))
    return f"{tag} {ts}  {size:>7}  {it.get('md5','')[:8]}  {it['server_filename']}"


def cmd_ls(at, args):
    rows = api_list(at, args.sub or "")
    if not rows:
        print(f"(空) {APP_DIR}/{(args.sub or '').strip('/')}")
        return
    for it in rows:
        print(fmt_row(it))


def cmd_tail(at, args):
    rows = [r for r in api_list(at) if not r.get("isdir")]
    rows.sort(key=lambda r: r.get("server_mtime", 0), reverse=True)
    for it in rows[: args.n]:
        print(fmt_row(it))
    if not rows:
        print("(沙盒目录暂无文件)")


def download(at: str, path: str, out_dir: Path) -> Path:
    meta = api_meta(at, path)
    dlink = meta.get("dlink")
    if not dlink:
        die("该文件无 dlink(可能是目录)。", 1)
    out_dir.mkdir(parents=True, exist_ok=True)
    dest = out_dir / Path(path).name
    with requests.get(
        dlink, params={"access_token": at}, headers={"User-Agent": UA},
        stream=True, timeout=TIMEOUT,
    ) as resp:
        if resp.status_code != 200:
            die(f"下载失败 HTTP {resp.status_code}", 3)
        with open(dest, "wb") as fh:
            for chunk in resp.iter_content(65536):
                fh.write(chunk)
    return dest


def cmd_get(at, args):
    path = resolve_path(args.name)
    dest = download(at, path, Path.cwd() / "bd_pull")
    print(dest.resolve())


def cmd_cat(at, args):
    path = resolve_path(args.name)
    dest = download(at, path, Path(os.environ.get("TMPDIR", "/tmp")) / "bdverify")
    sys.stdout.buffer.write(dest.read_bytes())


def cmd_md5(at, args):
    path = resolve_path(args.name)
    # The list/filemetas md5 field can be an opaque Baidu value rather than a
    # hexadecimal content digest.  Hash the bytes returned by the authenticated
    # download endpoint so this command is suitable for integrity comparison.
    dest = download(
        at,
        path,
        Path(os.environ.get("TMPDIR", "/tmp")) / "bdverify-md5",
    )
    digest = hashlib.md5()
    with dest.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    print(digest.hexdigest())


def cmd_rm(at, args):
    path = resolve_path(args.name)
    if not path.startswith(APP_DIR + "/"):
        die(f"路径不在应用沙盒内: {path}", 1)
    result = _post(
        f"{XPAN}/file",
        params={"method": "filemanager", "opera": "delete", "access_token": at},
        data={"async": "0", "filelist": json.dumps([path], ensure_ascii=False)},
    ).json()
    errno = result.get("errno", 0)
    item_errors = [item.get("errno", 0) for item in result.get("info", [])]
    if errno != 0 or any(item_errors):
        die(f"delete errno={errno}, item_errors={item_errors} ({result})", 3)
    print(f"deleted {path}")


def cmd_raw(at, args):
    print(f"ACCESS_TOKEN={at}")
    print(f"APP_DIR={APP_DIR}")
    print(f'# 例: curl -s "{XPAN}/file?method=list&dir={quote(APP_DIR)}&access_token=$ACCESS_TOKEN"')


def main() -> None:
    ap = argparse.ArgumentParser(prog="bdverify", description="网盘录音工牌 网盘沙盒闭环校验")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("auth", help="设备码授权")
    p = sub.add_parser("ls", help="列目录"); p.add_argument("sub", nargs="?", default="")
    p = sub.add_parser("tail", help="最近 n 个文件"); p.add_argument("n", nargs="?", type=int, default=1)
    p = sub.add_parser("get", help="下载到 ./bd_pull/"); p.add_argument("name")
    p = sub.add_parser("cat", help="下载并打印内容"); p.add_argument("name")
    p = sub.add_parser("md5", help="下载并计算真实内容 md5"); p.add_argument("name")
    p = sub.add_parser("rm", help="删除沙盒文件"); p.add_argument("name")
    sub.add_parser("raw", help="打印 token/目录")
    args = ap.parse_args()

    key, sec = load_creds()
    if args.cmd == "auth":
        do_auth(key, sec)
        return
    at = ensure_token(key, sec)
    {
        "ls": cmd_ls, "tail": cmd_tail, "get": cmd_get,
        "cat": cmd_cat, "md5": cmd_md5, "rm": cmd_rm, "raw": cmd_raw,
    }[args.cmd](at, args)


if __name__ == "__main__":
    main()
