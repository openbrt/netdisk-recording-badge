"""Exercise the real JPEG-to-BMP preview path without ESP hardware."""

import base64
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
# Synthetic 64x32 baseline JPEG: red left half, blue right half.
JPEG = (
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgICAgMCAgIDAwMDBAYEBAQEBAgGBgUGCQgKCgkICQkKDA8MCgsOCwkJDRENDg8QEBEQCgwSExIQEw8QEBD/2wBDAQMDAwQDBAgEBAgQCwkLEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBD/wAARCAAgAEADAREAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDxevxc/wBIAoAKACgDySv9fT/JIKACgAoA9br/ACCP9bQoAKACgDySv9fT/JIKACgAoA9br/II/wBbQoAKACgDySv9fT/JIKACgAoA9br/ACCP9bQoAKACgDySv9fT/JIKACgAoA//2Q=="
)


class ImageRenderTest(unittest.TestCase):
    def test_full_image_is_fitted_and_color_halves_survive(self):
        with tempfile.TemporaryDirectory() as d:
            directory = Path(d)
            source = directory / "input.jpg"
            dest = directory / "output.bmp"
            cli = directory / "render"
            source.write_bytes(base64.b64decode(JPEG))
            driver = directory / "driver.c"
            driver.write_text(
                '#include <stdlib.h>\n'
                'int kuku_image_render_bmp(const char *, const char *, int);\n'
                'int main(int argc, char **argv) { return argc == 4 ? '
                'kuku_image_render_bmp(argv[1], argv[2], atoi(argv[3])) != 0 : 2; }\n'
            )
            subprocess.run(
                ["cc", "-DLV_USE_TJPGD=1", "-DLV_CONF_SKIP=1",
                 str(ROOT / "main/kuku_image.c"),
                 str(ROOT / "managed_components/lvgl__lvgl/src/libs/tjpgd/tjpgd.c"),
                 str(driver), "-o", str(cli)],
                check=True, capture_output=True,
            )
            subprocess.run([str(cli), str(source), str(dest), "32"], check=True)
            bmp = dest.read_bytes()
            self.assertEqual(bmp[:2], b"BM")
            self.assertEqual(struct.unpack_from("<ii", bmp, 18), (32, 16))
            self.assertEqual(struct.unpack_from("<H", bmp, 28)[0], 24)
            pitch = (32 * 3 + 3) & ~3

            def rgb(x, y):
                off = 54 + (15 - y) * pitch + x * 3
                b, g, r = bmp[off:off + 3]
                return r, g, b

            left = rgb(2, 8)
            right = rgb(29, 8)
            self.assertGreater(left[0], 190)
            self.assertLess(left[2], 80)
            self.assertGreater(right[2], 190)
            self.assertLess(right[0], 80)


if __name__ == "__main__":
    unittest.main()
