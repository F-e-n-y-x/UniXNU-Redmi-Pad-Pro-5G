#!/usr/bin/env python3
"""Root overlay with PNG images in place of the HEIC ones of ruan's lock-screen wallpaper (Linux port of UniXNU
scripts/make-wallpaper-overlay.py).

  make-wallpaper-png.py ROOT OUTPUT_DIR [CA_BUNDLE ...]

ROOT is the root tree (make-ramroot.py's work-full/root, or a mounted OS volume). ruan carries the iPad7,1 identity, so
PosterBoard picks the Flat variant of wallpaper 7570 (Wallpaper.plist productTypes iPad7,1/iPad7,2; the layered variants list
them as unsupportedProductTypes). Its two images are HEIC, which ImageIO cannot decode without a hardware HEVC decoder.
CA_BUNDLE (relative to ROOT) defaults to that bundle; pass others (e.g. Lake's Flat bundle) to convert them too.
OUTPUT_DIR mirrors root paths: <bundle>/main.caml rewritten to point at <bundle>/<image>.png, and the PNG images, scaled to a
longest edge of EDGE pixels as UniXNU does (layers stretch their contents to their bounds). Feed it to make-ramroot.py --overlay.
Needs ImageMagick 7 (magick) with libheif.
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

DEFAULT = ('Library/Wallpaper/Collections/iPadOS_17~ipad.wallpaperCollection/Wallpapers/'
           '7570.iPadOS_17_Flat-1024w-1366h@2x~ipad.wallpaper/7570.iPadOS_17_Flat_Background-1024w-1366h@2x~ipad.ca')
EDGE = 2732   # the HEIC images are 3416 px square; UniXNU's size, keeps the root small


def convert(bundle_src, bundle_out):
    caml = (bundle_src / 'main.caml').read_text()
    names = sorted(set(re.findall(r'"/?assets/([^"]+)\.HEIC"', caml)))
    if not names:
        sys.exit(f'No HEIC image in {bundle_src}/main.caml')
    bundle_out.mkdir(parents=True)
    for name in names:
        png = name.replace('%20', '-').replace(' ', '-')
        heic = bundle_src / 'assets' / (name.replace('%20', ' ') + '.HEIC')
        # [0]: the primary image only (no gain map or thumbnail); '>' only shrinks
        subprocess.run(['magick', f'{heic}[0]', '-resize', f'{EDGE}x{EDGE}>', f'PNG32:{bundle_out / (png + ".png")}'], check=True)
        caml = caml.replace(f'"/assets/{name}.HEIC"', f'"/{png}.png"').replace(f'"assets/{name}.HEIC"', f'"{png}.png"')
    if re.search(r'\.HEIC"\s*/>', caml):
        sys.exit(f'{bundle_src}/main.caml still points at a HEIC image')
    (bundle_out / 'main.caml').write_text(caml)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    if not shutil.which('magick'):
        sys.exit('magick (ImageMagick 7 with HEIC) not found')
    root, out = Path(sys.argv[1]), Path(sys.argv[2])
    if out.exists():
        sys.exit('Refuse to overwrite ' + str(out))
    for rel in sys.argv[3:] or [DEFAULT]:
        if not (root / rel / 'main.caml').is_file():
            sys.exit('No wallpaper animation at ' + str(root / rel))
        convert(root / rel, out / rel)
    for path in sorted(p for p in out.rglob('*') if p.is_file()):
        size = subprocess.check_output(['magick', 'identify', '-format', '%wx%h', str(path)], text=True) if path.suffix == '.png' else ''
        print(f'{path.stat().st_size:>10} {size:>10} {path.relative_to(out)}')


if __name__ == '__main__':
    main()
