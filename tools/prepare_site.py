#!/usr/bin/env python3
"""Stage Pages from source and the exact approved GitHub Release binaries."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    release = json.loads((ROOT / 'site/release.json').read_text())
    output = ROOT / '_site'
    if output.exists():
        shutil.rmtree(output)
    shutil.copytree(ROOT / 'site', output, ignore=shutil.ignore_patterns('assets', 'firmware'))
    (output / 'assets').mkdir()
    shutil.copyfile(ROOT / 'assets/images/netdisk-recording-cover.png', output / 'assets/cover.png')
    firmware = output / 'firmware'
    firmware.mkdir()
    subprocess.run(['gh', 'release', 'download', release['tag'], '--repo', 'openbrt/netdisk-recording-badge',
                    '--dir', str(firmware), '--pattern', '*.bin'], check=True)
    expected = {item['name'] for item in release['files']}
    if {p.name for p in firmware.iterdir()} != expected:
        raise RuntimeError('Unexpected release asset set')
    for item in release['files']:
        data = (firmware / item['name']).read_bytes()
        if len(data) != item['bytes'] or hashlib.sha256(data).hexdigest() != item['sha256']:
            raise RuntimeError('Release identity mismatch: ' + item['name'])
        print('Verified release asset:', item['name'], item['sha256'])
    for name in ('manifest.json', 'manifest-update.json'):
        manifest = json.loads((output / name).read_text())
        if manifest['version'] != release['version']:
            raise RuntimeError('Manifest version mismatch')
        for build in manifest['builds']:
            for part in build['parts']:
                item = next(item for item in release['files'] if 'firmware/' + item['name'] == part['path'])
                if part['offset'] != item['offset']:
                    raise RuntimeError('Manifest offset mismatch')
    (output / '.nojekyll').touch()

if __name__ == '__main__':
    main()
