"""Generate fixed-UI Chinese glyph subsets with pinned lv_font_conv."""
import json
import re
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent.parent
assets = root / 'assets/fonts'
text = ''.join((root / name).read_text() for name in
               ('main/kuku_ui.c', 'main/main.c'))
# These modules display short status toasts. Include their string literals,
# without adding glyphs from unrelated implementation comments.
for name in ('main/kuku_baidu.c', 'main/kuku_rec.c'):
    source = (root / name).read_text()
    text += ''.join(re.findall(r'"(?:\\.|[^"\\])*"', source))
symbols = ''.join(sorted(set(re.findall(r'[\u3400-\u9fff\u3000-\u303f\uff00-\uffef…]', text))))
inventory = [ord(c) for c in symbols]
(assets / 'kuku_font_inventory.h').write_text(
    '#pragma once\nstatic const uint32_t kuku_font_inventory[] = {\n' +
    ','.join(hex(c) for c in list(range(0x20, 0x7f)) + inventory) + '\n};\n')
(assets / 'kuku-glyphs.json').write_text(json.dumps(inventory, indent=2) + '\n')
for size in (14, 20, 28):
    target = assets / f'kuku_font_{size}.c'
    subprocess.run(['npx', '--yes', 'lv_font_conv@1.5.3', '--font',
                    str(assets / 'SourceHanSansSC-Regular.otf'), '--range', '0x20-0x7e',
                    '--symbols', symbols, '--size', str(size), '--bpp', '4',
                    '--format', 'lvgl', '--no-compress', '--lv-include', 'lvgl.h',
                    '--lv-font-name', f'kuku_font_{size}', '--output', str(target)], check=True)
    covered = {int(x, 16) for x in re.findall(r'/\* U\+([0-9A-Fa-f]+)', target.read_text())}
    required = set(inventory) | set(range(0x20, 0x7f))
    missing = required - covered
    assert not missing, f'{size}px missing glyphs: {missing}'
    assert 0x9f98 not in covered, 'Negative coverage control failed'
    print(f'{size}px: PASS, {len(required)} glyphs, {target.stat().st_size} source bytes')
