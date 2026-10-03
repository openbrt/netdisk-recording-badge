#!/usr/bin/env python3
"""Focused host tests for bdverify path-to-fs_id metadata lookup."""

from __future__ import annotations

import importlib.util
import io
import json
import sys
import tempfile
import types
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch


MODULE_PATH = Path(__file__).with_name("bdverify.py")
SPEC = importlib.util.spec_from_file_location("bdverify", MODULE_PATH)
assert SPEC and SPEC.loader
bdverify = importlib.util.module_from_spec(SPEC)
with patch.dict(sys.modules, {"requests": types.ModuleType("requests")}):
    SPEC.loader.exec_module(bdverify)


class Response:
    def __init__(self, payload):
        self.payload = payload

    def json(self):
        return self.payload


class MetaLookupTests(unittest.TestCase):
    def test_resolves_path_to_fsid_before_filemetas(self):
        calls = []

        def fake_get(url, **kwargs):
            calls.append((url, kwargs["params"]))
            if kwargs["params"]["method"] == "list":
                return Response(
                    {
                        "errno": 0,
                        "list": [
                            {
                                "fs_id": 123456,
                                "path": f"{bdverify.APP_DIR}/REC0001.WAV",
                                "server_filename": "REC0001.WAV",
                            }
                        ],
                    }
                )
            return Response({"errno": 0, "list": [{"fs_id": 123456, "dlink": "https://d"}]})

        with patch.object(bdverify, "_get", side_effect=fake_get):
            meta = bdverify.api_meta("token", f"{bdverify.APP_DIR}/REC0001.WAV")

        self.assertEqual(meta["fs_id"], 123456)
        self.assertEqual(calls[1][1]["fsids"], "[123456]")
        self.assertNotIn("target", calls[1][1])
        self.assertNotIn("path", calls[1][1])

    def test_md5_hashes_downloaded_content(self):
        with tempfile.TemporaryDirectory() as td:
            payload = Path(td) / "REC0001.WAV"
            payload.write_bytes(b"cloud recording bytes")
            args = type("Args", (), {"name": "REC0001.WAV"})()
            output = io.StringIO()
            with patch.object(bdverify, "download", return_value=payload):
                with redirect_stdout(output):
                    bdverify.cmd_md5("token", args)
        self.assertEqual(output.getvalue().strip(), "3996341b06afce0ab166ed1e8a6ba9d6")

    def test_rm_posts_sandbox_path(self):
        args = type("Args", (), {"name": "REC0001.WAV"})()
        response = Response({"errno": 0, "info": [{"errno": 0}]})
        output = io.StringIO()
        with patch.object(bdverify, "_post", return_value=response) as post:
            with redirect_stdout(output):
                bdverify.cmd_rm("token", args)

        kwargs = post.call_args.kwargs
        self.assertEqual(kwargs["params"]["opera"], "delete")
        self.assertEqual(
            json.loads(kwargs["data"]["filelist"]),
            [f"{bdverify.APP_DIR}/REC0001.WAV"],
        )
        self.assertIn("deleted", output.getvalue())


if __name__ == "__main__":
    unittest.main()
