"""Protect the measured ESP32-C3 upload-task stack requirement."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BaiduUploadStackTest(unittest.TestCase):
    def test_upload_task_keeps_measured_snprintf_headroom(self):
        source = (ROOT / "main/kuku_baidu.c").read_text()
        match = re.search(r"#define KUKU_BD_UPLOAD_STACK\s+(\d+)", source)
        self.assertIsNotNone(match)
        self.assertGreaterEqual(int(match.group(1)), 12 * 1024)
        self.assertRegex(
            source,
            r'xTaskCreate\(upload_task,\s*"kuku_bd_up",\s*'
            r"KUKU_BD_UPLOAD_STACK,",
        )


if __name__ == "__main__":
    unittest.main()
